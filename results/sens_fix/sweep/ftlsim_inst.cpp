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
    std::string out_ts;
    std::string trace_file;       // replay a real trace (CSV: dev,op,byte_off,len_bytes[,ts]) instead of the synthetic workload
    double trace_warmup = 0.5;    // fraction of the trace's write volume used as aging; stats reset there, rest is measured
    std::string out_trace;        // export the post-buffer host stream (one line per 4K page: "0 1 <lpn> 4096")           // csv path for time-series
    std::string tag;              // free-form label echoed in output
    bool fine_extra = true;       // fine quota is physical space ADDED on top of OP (else carved from OP)
    bool gc_frontier = true;      // separate active blocks for GC/reclaim writes vs host writes (hot/cold separation)
    bool zipf_shuffle = false;
    std::string fine_index = "hash"; // proposed RAM model of the fine map: hash (per-IU entry, open addressing, LF 0.7, + 4 B/page slots)
                                     // | cuckoo (per-page entries in a bucketized cuckoo table: 4-way buckets, 2 hashes, LF 0.95;
                                     //   entry = lpn 4 B + ppn 4 B (+ 4 B write time when the cold test needs per-IU recency))
    int rec_block = 0;               // 1: per-IU recency from block metadata instead of a per-entry write time (8 B fine entries):
                                     //    an IU is "recent" iff one of its fragments sits in a host-frontier fine block born later
    int uni_onefront = 0;            // adapt uniform mode: fine GC copies share the host fine frontier (one fewer open block)
    double map_budget = 0;           // fine_index=cuckoo2: total mapping DRAM as a fraction of the page map (L x 4 B);
                                     // the fine table gets what the coarse map leaves: 2-choice 4-way buckets, statically sized
    std::string fine_cap = "blocks"; // proposed: blocks (fixed fine block quota) | ram (no block quota; fine map capped by RAM budget, global greedy GC across tiers)
    u64 fine_budget_bytes = 0;    // mixed/ram: RAM budget for the fine map (0 = 10% of page4k L2P minus coarse map)
    std::string fine_policy = "cost";  // fine victim handling: cost (default, parameter-free: fold IU iff 16/d < v/(P-v)) | cold (pressure-gated coldest-first) | greedy (d>=fold_d) | block (whole-block cost test)
                                       // | fifo_fold (oldest fine block, fold all) | greedy_copy (min-valid block, copy only) | alt (alternate fifo_fold / greedy_copy 1:1)
                                       // | cost (per-IU, parameter-free: fold iff 16/d < v/(P-v)) | cost_cold (cost + fine-cold filter)
    std::string unfold = "off";   // coarse GC: off | hot (unfold hot IUs) | any (space-driven: unfold whenever the fine pool has room, incl. fragment-less IUs)
    double unfold_tau = 0.5;      // hot if last fragment write within unfold_tau x (fine tier capacity) of fine host writes
    int unfold_k = 2;             // ... and at least unfold_k host fragment writes within that window (frequency; 1 = recency only)
    int eager_u = -1;             // proposed: -1 = cost-based (default) (see eager_worth); >0 = fixed threshold: if the IU would end up with > eager_u
                                  // of its 16 pages present (new pages + existing fragments), complete it to a full
                                  // chunk right away (selective RMW) instead of appending fragments. 0 = off.
    int fold_d = 8;               // greedy/cold: fold an IU only when it has >= fold_d fragments (half a chunk)
    double fold_tau = -1;         // cost_cold: <0 = parameter-free (cold iff untouched while the victim block lived);
                                  // >=0 = legacy fixed window of fold_tau x (fine tier capacity)
    bool fine_full = false;       // full-IU host writes to fine-RESIDENT IUs stay in the fine pool (keep hot IUs page-mapped)
    std::string gc_select = "coarse"; // global free-block source: coarse (always coarse GC, default) | cost (pick coarse vs fine victim by v/(P-v))
    int coarse_class = 0;         // SepBIT-style age cascade for coarse GC survivors: N extra GC frontiers (0 = off, one shared GC frontier)
    std::string cgc_vict = "greedy"; // coarse GC victim: greedy (min valid) | cb (cost-benefit: max (P-v)/v * sqrt(age))       // full-IU host writes to fine-RESIDENT IUs stay in the fine pool (keep hot IUs page-mapped)
    double fold_gate = 0.0;       // cold policy: fold only when fine victim valid ratio v/P >= fold_gate (fine-pressure gate; 0 = always)
    // FAST / LAST baselines
    double last_seq_frac = 0.25;  // fraction of log blocks used as sequential log buffer (LAST)
    u64 last_seq_thresh = 4;      // request size (pages) > thresh -> sequential log buffer (LAST; paper 4KB@2KB pages; 16KB best here)
    double last_hot_frac = 0.5;   // initial hot partition fraction of the random log buffer (LAST)
    bool last_fixed_caps = true;  // disable the paper's reclaimed-block reassignment (fixed partition sizes)
    u64 cache_bytes = 12ull << 20; // dftl / mu: RAM for cached mapping (translation-page cache / mu-tree cache)
    double leaftl_gamma = 1e-4;   // leaftl: PLR error bound (artifact default 1e-4 = exact segments)
    int leaftl_batch = 2048;      // leaftl: pages sorted by LPN per flash-allocation batch (artifact RWCache flush = 8 MiB)
    bool leaftl_gcsort = true;    // leaftl: GC relocates a victim's valid pages in LPN order and relearns them
    int adapt = 0;                // proposed: 1/2 = switch per fine-pool turnover between the canonical policy and a
                                  // uniform mode (fine pool = all spare OP, greedy coarse victim, no age cascade) when the
                                  // fine pool absorbs less than half of its host pages by overwrite (1: majority rule) or no
                                  // more than a random cache of its size would (2: absorption < pool pages / logical pages)
    bool dftl_tpbuf = false;      // dftl: keep the last translation page read from flash in controller SRAM, so back-to-back
                                  // misses (and write-backs) on the same page cost one flash read (sequential runs no longer pay per entry)
    u64 bitmap_cache_bytes = 32768; // mu: bitmap (invalid-extent) cache
    int mu_parts = 8;             // mu: logical address space partitions (each has its own update block)
    int mu_max_extent = 128;      // mu: max extent length (pages)
    int mu_leaf_fanout = 256;     // mu: extents per mu-tree leaf page (one 4K page holds the path; leaf ~ half)
    double janus_u = 0.7;         // janus: PMA utilization target (defuse when above, else GC)
    int janus_cap = 0;            // janus: max fused blocks (0 = from 10% RAM budget; -1 = unlimited)
    u64 last_k = 0;               // hot update-interval threshold in pages (0 = auto: hot partition capacity)    // true: random rank->IU permutation (hot IUs scattered); false: rank==IU (hot region contiguous)
    // ---- partially filled device (file-system-like): defaults reproduce the full-device workload exactly
    double fill_frac = 1.0;       // seq fill writes only the first fill_frac of the LBA space (the rest is never written)
    double foot_frac = 1.0;       // random/zipf workload writes only over the first foot_frac of the IUs
    double app_frac = 0.0;        // fraction of workload pages issued by sequential append streams (WAL-like)
    u64 app_region = 1ull << 30;  // bytes of the (circular) append region, split evenly among the streams
    double app_off = -1;          // append region start as a fraction of L (<0: right after the footprint)
    int app_k = 1;                // append request = 2^app_k pages
    int app_rep = 1;              // each append request is issued app_rep times (WAL tail-page rewrites) before advancing
    int app_streams = 1;          // number of interleaved append streams (e.g. one WAL per database cluster)
    // ---- elastic fine pool (proposed): see Sim::elastic_quota()
    int fine_elastic = 0;         // 0 = fixed quota (default). 1 = fine quota grows into physical space the coarse tier
                                  //     does not need (never-written / freed LBA space), shrinking back as coarse data grows
    double elastic_keep = 0;      // coarse headroom kept free of fine blocks, as a fraction of coarse-resident pages
                                  //     (0 = auto: OP fraction, i.e. the coarse tier keeps at least its nominal OP ratio)
    int evict_pol = 0;            // cuckoo2 eviction among the 8 candidates: 0 = most fragments (then coldest),
                                  //     1 = max fragments x age, 2 = coldest (then most fragments)
    // ---- trace replay with original addresses (block traces captured on the 44G NVMeVirt device)
    int trace_remap = 1;          // 1 = dense first-touch 1 MiB remap (default); 0 = identity (LBA = byte_off / 4096; must fit in L)
    long long trace_warmup_pages = -1; // >= 0: absolute write pages before the measurement starts (overrides trace_warmup)
    long long trace_stop_pages = -1;
    int trace_clip = 0;
    int dram_stats = 0;           // 1 = count mapping-structure DRAM accesses (cache-line touches) per host read / host write /
                                  //     GC page / fold page; also replays trace reads (and, in synthetic mode, one read probe per
                                  //     write request from the write distribution) through each policy's read lookup. Reads change
                                  //     DFTL / LeaFTL cache state, so WAF from a dram_stats run is not the reference WAF.           // trace_remap=0: 1 = drop write pages beyond L (L is rounded down to a multiple of U) instead of dying
    double gc_fine_bias = 1.0;    // sensitivity knob (study only): gc_select=cost reclaims the fine victim iff v_f < bias x v_c   // >= 0: stop the replay after this many write pages (end-of-measurement marker)
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
    u64 fine_host_new = 0, fine_host_ow = 0, uni_host = 0;
    u64 fine_peak = 0;
    u64 n_evict_fold = 0;                    // cuckoo2: folds forced by a full bucket pair (the table is the DRAM budget)                       // max fine-resident valid pages, sampled at each fine block allocation // host pages entering the fine pool as a new fragment vs. overwriting a fine-resident page
    u64 host_fine = 0, host_coarse = 0;  // host pages by tier
    u64 n_unfold = 0, unfold_pages = 0; std::vector<u64> d_unfold;
    u64 n_budget_fold = 0, n_gcfold = 0;
    u64 n_shrink_fold = 0, n_space_capped = 0;
    u64 vblk_age_sum = 0, vblk_age_n = 0;   // victim fine block age at reclaim (fine_clock units)
    u64 vblk_age_hist[6] = {0,0,0,0,0,0};   // in pool turnovers: <.25 .5 1 2 4 4+
    u64 age_hist[7] = {0,0,0,0,0,0,0};   // (fine_clock-flast)/pool turnovers: <.125 .25 .5 1 2 4 4+
    u64 age_hist_fold[7] = {0,0,0,0,0,0,0};  // ... among those the cost test wanted to fold
    u64 n_switch = 0, n_partial = 0, n_full = 0, n_dead_reclaim = 0, n_merge_reclaim = 0, full_assoc_sum = 0, n_seq_req = 0, n_rand_req = 0, n_hot_pages = 0, n_cold_pages = 0;
    // diagnostics (report only): fine victim valid at greedy/cost reclaim, folds by reason, pool occupancy at erases
    u64 greedy_v_sum = 0, n_greedy_fold = 0, n_shrink_reclaim = 0, shrink_v_sum = 0, n_elastic_shrink = 0;
    u64 occ_n = 0; double occ_fv = 0, occ_fb = 0, occ_free = 0, occ_cv = 0, occ_quota = 0;
    // dram_stats: contexts 0 host read, 1 host write, 2 GC (relocation), 3 fold (incl. cuckoo2 eviction folds)
    u64 dacc[4] = {0, 0, 0, 0}, dfr[4] = {0, 0, 0, 0};   // DRAM accesses / flash map reads (translation pages, LeaFTL frames, mispredictions)
    u64 d_rd_ops = 0, d_rd_max = 0, d_rd_hist[16] = {0}, d_rd_fr_max = 0, d_rd_buf = 0, d_rd_unmapped = 0;
    u64 d_wp_ops = 0, d_wp_max = 0, d_wiu_ops = 0, d_wiu_max = 0, d_wiu_pages = 0, d_evict = 0, d_evict_acc = 0, d_evict_max = 0, d_lf_mispredict = 0;
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
    // adapt=1 state: canonical quota vs. uniform-mode quota (all spare OP), absorption window counters
    int fine_quota_base = 0, fine_quota_uni = 0; bool uni = false;
    u64 aw_new = 0, aw_ow = 0, n_mode_switch = 0;
    int cc_eff() const { return uni ? 0 : c.coarse_class; }
    // ---- fine_elastic: the fine pool may grow beyond its quota into physical space the coarse tier does not need.
    // Coarse need = live chunks x U (what the coarse tier occupies when compacted; never-written IUs own nothing),
    // plus headroom, plus its open frontiers and the GC reserve. O(1): one counter of live chunks.
    u64 n_chunks = 0;                 // IUs with a live coarse chunk
    u64 fpages = 0;                   // fine-resident valid pages (= fine table entries)
    // live coarse pages: chunk slots minus the pages fragments supersede (as L - fv for a full device)
    u64 coarse_live() const { u64 a = n_chunks * (u64)U; return a > fpages ? a - fpages : 0; }
    int fine_cap() const {
        if (!c.fine_elastic) return fine_quota_blocks;
        double keep = c.fine_elastic == 2 ? 1.0 / 32 : (c.elastic_keep > 0 ? c.elastic_keep : c.op);
        u64 need = (u64)std::ceil((double)coarse_live() * (1.0 + keep) / P);
        long cap = (long)NB - (long)need - (2 + cc_eff()) - c.T - 1;
        return (int)std::max<long>(fine_quota_blocks, cap);
    }

    // maps
    std::vector<u32> coarse_map;   // IU -> extent start ppn
    std::vector<u32> pmap;         // page4k: lpn -> ppn
    std::vector<int32_t> fine_idx; // IU -> fine pool index or -1
    std::vector<u32> fbitmap;      // pool idx -> bitmap
    std::vector<u32> fslot;        // pool idx*U + off -> ppn
    std::vector<u64> fbirth;       // fine block -> fine_clock when the block was opened (parameter-free cold test)
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
    static const int MAXCC = 4;
    Frontier ccf[MAXCC];           // coarse_class: cascade frontiers for GC survivors (class 1..coarse_class)
    std::vector<int8_t> bccls;     // per-block coarse age class (0 = host/refold, 1..N = GC-survivor generation)
    int cgc_cls = 0;               // class of the coarse block currently being GC'd
    // ---- SepBIT (Wang et al., FAST'22) 6-class placement on the page map (reference: github fallfish/sepbit)
    Frontier sbf[6];               // one open segment per class
    std::vector<u64> sb_wtime;     // per-LPN user-write timestamp (updated on host writes only, like Metadata)
    std::vector<u64> sb_birth;     // per-block birth timestamp
    std::vector<int8_t> sb_class;  // per-block class
    double sb_avg = 1e300;         // mAvgLifespan (initially "infinite")
    u64 sb_now = 0;                // global timestamp = cumulative host pages (GC appends do not tick)
    u64 sb_lifesum = 0; int sb_ncollect = 0;
    bool in_cgc = false, in_fine_reclaim = false;
    bool fold_allowed = true;      // cold policy: set per victim by the pressure gate
    double last_fine_v = 0;        // EWMA of the fine victim's valid pages (fine-tier pressure signal)
    // ---- mixed mode: one pool (no fine/coarse block tags), fine map capped by RAM budget, single greedy GC
    bool mixed = false;
    bool ram_cap = false;
    bool in_shrink = false;
    int reclaim_victim = -1;
    int last_vblk = -1;          // proposed + fine_cap=ram
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
            fine_quota_base = fine_quota_blocks;
            {   // uniform-mode quota: every block not needed for the logical data, the GC reserve and the
                // four open frontiers (host/GC x fine/coarse) -- geometry only, no tuning constant
                int data_blocks = (int)((L + P - 1) / P);
                fine_quota_uni = std::max(fine_quota_base, NB - data_blocks - c.T - (c.uni_onefront ? 3 : 4) - 1);
            }
            if (c.fine_index == "cuckoo2") {
                if (c.unfold != "off") DIE("cuckoo2 does not support unfold");
                double ent = 4 + 4 + (!c.rec_block && (c.fine_policy == "cold" || c.fine_policy == "cost_cold") ? 4 : 0);
                double fine_bytes = c.map_budget * (double)L * 4 - (double)(L / U) * 4;
                if (fine_bytes < 4 * ent * 16) DIE("map_budget %.4f leaves no room for the fine table (coarse map alone is %.4f of the page map)", c.map_budget, 1.0 / U);
                ct_nb = (u64)(fine_bytes / (4 * ent)); ct.assign(ct_nb * 4, NONE); ct_on = true;
                fprintf(stderr, "[cfg] cuckoo2 fine table: %llu buckets x 4 = %llu entries (%.2f MiB), budget %.4f of page map\n",
                        (unsigned long long)ct_nb, (unsigned long long)(ct_nb * 4), ct_nb * 4 * ent / (1 << 20), c.map_budget);
            }
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
        bccls.assign(NB + 1, 0);
        fbirth.assign(NB + 1, 0); bhost.assign(NB + 1, 0);
        if (is_pg()) { pmap.assign(L, NONE); cmt_cap = (u64)(c.cache_bytes * HASH_LF / 8.0); }
        if (is_sb()) { sb_wtime.assign(L, ~0ull); sb_birth.assign(NB + 1, 0); sb_class.assign(NB + 1, 1); }
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
    bool is_pg() const { return c.mode == "page4k" || c.mode == "dftl" || c.mode == "sepbit" || c.mode == "leaftl"; }
    bool is_lf() const { return c.mode == "leaftl"; }
    bool is_sb() const { return c.mode == "sepbit"; }
    // ---- dram_stats: one access = one random cache-line touch of a mapping structure (L2P / CMT / cuckoo bucket /
    // IU bitmap / coarse map / LeaFTL frame directory or segment). Block-level metadata (valid counters, P2L in OOB) is
    // common to every policy and not counted.
    int dctx = 1;
    void DA(u64 n = 1) { if (c.dram_stats) st.dacc[dctx] += n; }
    void MR() { st.read_map++; if (c.dram_stats) st.dfr[dctx]++; }
    struct DCtx { Sim* s; int saved; DCtx(Sim* sp, int x) : s(sp), saved(sp->dctx) { s->dctx = x; } ~DCtx() { s->dctx = saved; } };

    // ---- DFTL (Gupta et al., ASPLOS'09) cached page mapping (CFTL's write path is identical): tier-2 translation pages (1024 entries) in flash, CMT in RAM (LRU).
    static const int TP_ENTRIES = 1024;
    u64 cmt_cap = 0;
    u64 tp_buf = ~0ull;                                         // dftl_tpbuf: translation page currently held in SRAM
    std::list<u64> cmt_lru;                                     // front = MRU
    std::unordered_map<u64, std::pair<std::list<u64>::iterator, bool>> cmt;  // lpn -> (lru pos, dirty)
    void cmt_evict_one() {
        u64 victim = cmt_lru.back(); cmt_lru.pop_back();
        auto it = cmt.find(victim); bool dirty = it->second.second; cmt.erase(it);
        DA(1);
        if (!dirty) return;
        // batch update: write back the translation page, cleaning every cached dirty entry of the same page
        u64 tp = victim / TP_ENTRIES;
        if (!c.dftl_tpbuf || tp != tp_buf) MR();
        if (c.dftl_tpbuf) tp_buf = tp;
        st.prog[S_MAP]++; st.map_flush++;
        u64 cleaned = 0;
        for (u64 l = tp * TP_ENTRIES; l < (tp + 1) * TP_ENTRIES; ++l) { auto j = cmt.find(l); if (j != cmt.end()) { j->second.second = false; cleaned++; } }
        DA(cleaned);
    }
    // mapping update for lpn on a host write (DFTL: miss -> read translation page, insert; dirty)
    void cmt_update(u64 lpn) {
        if (c.mode != "dftl") return;
        auto it = cmt.find(lpn);
        DA(1);                                                  // CMT probe (+ in-place update / LRU relink)
        if (it != cmt.end()) { cmt_lru.erase(it->second.first); cmt_lru.push_front(lpn); it->second = {cmt_lru.begin(), true}; return; }
        DA(1);                                                  // GTD entry of the translation page
        st.map_miss++;
        u64 tp = lpn / TP_ENTRIES;
        if (!c.dftl_tpbuf || tp != tp_buf) MR();
        while (cmt.size() >= cmt_cap) cmt_evict_one();
        if (c.dftl_tpbuf) tp_buf = tp;
        cmt_lru.push_front(lpn); cmt[lpn] = {cmt_lru.begin(), true};
    }
    // GC relocation: cached entries are updated in place; others update their translation pages in batch
    std::unordered_set<u64> gc_tps;
    void cmt_gc_update(u64 lpn) {
        if (c.mode != "dftl") return;
        auto it = cmt.find(lpn);
        DA(1);
        if (it != cmt.end()) { it->second.second = true; return; }
        DA(1);
        gc_tps.insert(lpn / TP_ENTRIES);
    }
    void cmt_read(u64 lpn) {
        auto it = cmt.find(lpn);
        DA(1);
        if (it != cmt.end()) { cmt_lru.erase(it->second.first); cmt_lru.push_front(lpn); it->second.first = cmt_lru.begin(); return; }
        DA(1);
        st.map_miss++;
        u64 tp = lpn / TP_ENTRIES;
        if (!c.dftl_tpbuf || tp != tp_buf) MR();
        while (cmt.size() >= cmt_cap) cmt_evict_one();
        if (c.dftl_tpbuf) tp_buf = tp;
        cmt_lru.push_front(lpn); cmt[lpn] = {cmt_lru.begin(), false};
    }
    void cmt_gc_flush() { for (u64 tp : gc_tps) { (void)tp; MR(); st.prog[S_MAP]++; st.map_flush++; } gc_tps.clear(); }


    // ---- LeaFTL (Sun et al., ASPLOS'23; port of platformxlab/LeaFTL wiscsim/learnedftl.py).
    // Page-level log-structured data layout (shared frontiers / greedy GC). The L2P table is a set of learned
    // segments per 256-LPN frame: greedy PLR with error bound gamma over (lpn, ppn) pairs that were allocated in
    // LPN-sorted batches. Frames are cached in DRAM (LRU, budget = cache_bytes, memory per the artifact's formula:
    // single point 5 B, consecutive 8 B, non-consecutive 8 B + 1 B per bitmap run boundary; +4 B per frame);
    // evicted dirty frames cost one translation-page program, touching a flash-resident frame costs one flash read.
    // Differences from the artifact, all in LeaFTL's favour or neutral: shadowed segment coverage is removed at
    // insert time (always-compacted table), clean evictions are free, write-path mispredictions are not charged,
    // and GC relearns relocated pages (the artifact never updates the learned table on GC).
    static const int LF_FRAME = 256;
    struct LSeg { u32 x1, x2; double k, b; bool accurate, consec; u64 cov[4]; };
    struct LFrame { std::vector<LSeg> segs; bool resident = false, dirty = false, partial = false; std::list<u64>::iterator lru; double mem = 0; };
    std::unordered_map<u64, LFrame> lframes;
    std::list<u64> lf_lru;                      // resident frames, front = MRU
    double lf_cached = 0;                       // bytes of resident frames
    std::vector<u32> lf_stage; std::unordered_set<u32> lf_stage_set;
    u64 lf_mispredict = 0, lf_lookups = 0;
    static double f16(double v) { if (v == 0) return 0; int e; double m = std::frexp(v, &e); return std::ldexp(std::nearbyint(std::ldexp(m, 11)), e - 11); }
    static bool cov_get(const u64* c, int i) { return (c[i >> 6] >> (i & 63)) & 1; }
    static int cov_first(const u64* c) { for (int w = 0; w < 4; ++w) if (c[w]) return w * 64 + __builtin_ctzll(c[w]); return -1; }
    static int cov_last(const u64* c) { for (int w = 3; w >= 0; --w) if (c[w]) return w * 64 + 63 - __builtin_clzll(c[w]); return -1; }
    static double seg_mem(const LSeg& g, u64 base) {
        if (g.x1 == g.x2) return 1 + 4;                    // SUBLPN(1) + PPN(4)
        if (g.consec) return 1 + 4 + 2 + 1;                // + float16 slope + length
        int lo = (int)(g.x1 - base), hi = (int)(g.x2 - base), bnd = 0;
        for (int i = lo + 1; i < hi; ++i) if (cov_get(g.cov, i) != cov_get(g.cov, i - 1)) bnd++;
        return 1 + 4 + 2 + 1 + bnd;
    }
    // greedy PLR, same state machine as the artifact's PLR.process/build_segment
    struct PL { double k, b; };
    static PL pl_from(double x1, double y1, double x2, double y2) { double k = (y2 - y1) / (x2 - x1); return {k, y1 - k * x1}; }
    void lf_emit(std::vector<LSeg>& out, const std::vector<std::pair<u64,u64>>& pts, int state, double k, double b, u64 base) {
        LSeg g; g.x1 = (u32)pts.front().first; g.x2 = (u32)pts.back().first;
        if (state == 1) { g.k = 1; g.b = (double)pts[0].second - (double)pts[0].first; } else { g.k = k; g.b = b; }
        g.accurate = true; g.consec = true;
        std::memset(g.cov, 0, sizeof g.cov);
        for (size_t i = 0; i < pts.size(); ++i) {
            if (std::llround((double)pts[i].first * g.k + g.b) != (long long)pts[i].second) g.accurate = false;
            if (i >= 2 && pts[i].first - pts[i-1].first != pts[1].first - pts[0].first) g.consec = false;
            int r = (int)(pts[i].first - base); g.cov[r >> 6] |= 1ull << (r & 63);
        }
        out.push_back(g);
    }
    std::vector<LSeg> lf_learn(const std::vector<std::pair<u64,u64>>& pts, u64 base) {
        std::vector<LSeg> out; std::vector<std::pair<u64,u64>> cur;
        const double gm = c.leaftl_gamma;
        int state = 0; double s0x = 0, s0y = 0, sx = 0, sy = 0; PL lo{0,0}, up{0,0};
        for (auto& p : pts) {
            double x = (double)p.first, y = (double)p.second;
            if (state == 0) { s0x = x; s0y = y; state = 1; }
            else if (state == 1) {
                lo = pl_from(s0x, s0y + gm, x, y - gm); up = pl_from(s0x, s0y - gm, x, y + gm);
                sx = (lo.b - up.b) / (up.k - lo.k); sy = (up.k * lo.b - lo.k * up.b) / (up.k - lo.k);   // intersection(rho_upper, rho_lower)
                state = 2;
            } else {
                bool out_of = !(y > lo.k * x + lo.b) || !(y < up.k * x + up.b);
                if (out_of) {
                    double k = f16((lo.k + up.k) / 2.0);
                    lf_emit(out, cur, 2, k, -sx * k + sy, base);
                    cur.clear(); s0x = x; s0y = y; state = 1;
                } else {
                    if (y + gm < up.k * x + up.b) up = pl_from(sx, sy, x, y + gm);
                    if (y - gm > lo.k * x + lo.b) lo = pl_from(sx, sy, x, y - gm);
                }
            }
            cur.push_back(p);
        }
        if (state == 1) lf_emit(out, cur, 1, 0, 0, base);
        else if (state == 2) { double k = f16((lo.k + up.k) / 2.0); lf_emit(out, cur, 2, k, -sx * k + sy, base); }
        return out;
    }
    void lf_set_mem(LFrame& f, u64 fno) {
        double m = 4; u64 base = fno * LF_FRAME;
        for (auto& g : f.segs) m += seg_mem(g, base);
        if (f.resident) lf_cached += m - f.mem;
        f.mem = m;
    }
    void lf_make_resident(u64 fno, LFrame& f, bool load) {
        if (f.resident) { lf_lru.erase(f.lru); lf_lru.push_front(fno); f.lru = lf_lru.begin(); return; }
        if (load) { st.map_miss++; MR(); f.partial = false; }
        else if (!f.segs.empty()) f.partial = true;          // update without loading: merged (read) at eviction
        f.resident = true; lf_lru.push_front(fno); f.lru = lf_lru.begin(); lf_cached += f.mem;
    }
    void lf_enforce() {
        while (lf_cached > (double)c.cache_bytes && lf_lru.size() > 1) {
            u64 v = lf_lru.back(); lf_lru.pop_back();
            LFrame& f = lframes[v];
            DA(1);
            if (f.dirty) { if (f.partial) MR(); st.prog[S_MAP]++; st.map_flush++; }
            f.resident = false; f.dirty = false; f.partial = false; lf_cached -= f.mem;
        }
    }
    // mapping lookup needed to invalidate a page's previous copy (write path) or serve a read
    u64 lf_scan_cost(u64 lpn) {
        u64 fno = lpn / LF_FRAME; auto it = lframes.find(fno);
        if (it == lframes.end()) return 1;
        int r = (int)(lpn - fno * LF_FRAME); u64 n = 1;
        for (auto& g : it->second.segs) { n++; if (cov_get(g.cov, r)) break; }
        return n;
    }
    void lf_touch_lookup(u64 lpn) {
        if (c.dram_stats) DA(lf_scan_cost(lpn));
        u64 fno = lpn / LF_FRAME; LFrame& f = lframes[fno];
        lf_lookups++;
        if (!f.resident) lf_make_resident(fno, f, true);
        else {
            if (f.partial) { st.map_miss++; MR(); f.partial = false; }   // merge with the flash copy
            lf_make_resident(fno, f, false);
        }
    }
    void lf_insert(const std::vector<std::pair<u64,u64>>& pairs) {   // pairs sorted by lpn, all current
        size_t i = 0;
        while (i < pairs.size()) {
            u64 fno = pairs[i].first / LF_FRAME; size_t j = i;
            while (j < pairs.size() && pairs[j].first / LF_FRAME == fno) ++j;
            std::vector<std::pair<u64,u64>> pts(pairs.begin() + i, pairs.begin() + j);
            std::vector<LSeg> ns = lf_learn(pts, fno * LF_FRAME);
            LFrame& f = lframes[fno];
            lf_make_resident(fno, f, false);
            u64 nc[4] = {0, 0, 0, 0};
            for (auto& g : ns) for (int w = 0; w < 4; ++w) nc[w] |= g.cov[w];
            std::vector<LSeg> keep; keep.reserve(f.segs.size() + ns.size());
            for (auto& g : ns) keep.push_back(g);
            for (auto& g : f.segs) {                            // older segments lose the coverage the new ones shadow
                for (int w = 0; w < 4; ++w) g.cov[w] &= ~nc[w];
                int a = cov_first(g.cov); if (a < 0) continue;
                g.x1 = (u32)(fno * LF_FRAME + a); g.x2 = (u32)(fno * LF_FRAME + cov_last(g.cov));
                keep.push_back(g);
            }
            f.segs.swap(keep); f.dirty = true;
            DA(1 + f.segs.size());                              // rewrite the frame's segment list
            lf_set_mem(f, fno);
            i = j;
        }
        lf_enforce();
    }
    void lf_flush_stage() {
        if (lf_stage.empty()) return;
        std::sort(lf_stage.begin(), lf_stage.end());
        std::vector<std::pair<u64,u64>> pairs; pairs.reserve(lf_stage.size());
        for (u32 lpn : lf_stage) {
            if (pmap[lpn] != NONE) lf_touch_lookup(lpn);
            page_write(lpn, S_HOST);
            pairs.push_back({lpn, pmap[lpn]});
        }
        lf_stage.clear(); lf_stage_set.clear();
        std::vector<std::pair<u64,u64>> cur;                    // GC inside the batch may have moved (and relearned) some pages
        for (auto& p : pairs) if (pmap[p.first] == p.second) cur.push_back(p);
        lf_insert(cur);
    }
    void lf_host(u64 lpn) {
        if (lf_stage_set.count((u32)lpn)) lf_flush_stage();    // never absorb a rewrite inside the sort batch
        lf_stage.push_back((u32)lpn); lf_stage_set.insert((u32)lpn);
        if ((int)lf_stage.size() >= c.leaftl_batch) lf_flush_stage();
    }
    // predicted PPN for lpn (verification / read path); returns NONE if not covered
    u64 lf_predict(u64 lpn, bool* accurate) {
        u64 fno = lpn / LF_FRAME; auto it = lframes.find(fno);
        if (it == lframes.end()) return NONE;
        int r = (int)(lpn - fno * LF_FRAME);
        for (auto& g : it->second.segs) if (cov_get(g.cov, r)) { *accurate = g.accurate; return (u64)std::llround((double)lpn * g.k + g.b); }
        return NONE;
    }
    void lf_verify() {
        u64 bad = 0, miss = 0, approx = 0;
        for (u64 l = 0; l < L; ++l) {
            if (pmap[l] == NONE) continue;
            bool acc = true; u64 pr = lf_predict(l, &acc);
            if (pr == NONE) miss++;
            else if (pr != pmap[l]) { if (acc) bad++; else approx++; }
        }
        if (bad || miss) DIE("leaftl verify: %llu wrong accurate predictions, %llu uncovered LPNs", (unsigned long long)bad, (unsigned long long)miss);
        fprintf(stderr, "[verify] leaftl ok: approx-mispredict=%llu frames=%zu resident=%zu cached=%.2f MiB\n",
                (unsigned long long)approx, lframes.size(), lf_lru.size(), lf_cached / (1 << 20));
    }

    // adapt=1: after each canonical fine-pool turnover of host fine pages, pick the mode by majority rule
    void adapt_tick(bool ow) {
        if (ow) aw_ow++; else aw_new++;
        if (aw_ow + aw_new < (u64)fine_quota_base * P) return;
        // adapt=1: majority rule (absorbs less than half); adapt=2: absorbs no more than a random cache of the
        // fine pool's size would under uniform writes (expected absorption = pool pages / logical pages)
        bool u = c.adapt == 2 ? (double)aw_ow * (double)L < (double)(aw_ow + aw_new) * (double)fine_quota_base * P
                              : 2 * aw_ow < aw_ow + aw_new;
        aw_ow = aw_new = 0;
        if (u == uni) return;
        uni = u; n_mode_switch++;
        fine_quota_blocks = uni ? fine_quota_uni : fine_quota_base;
        if (uni && c.uni_onefront && fgf.blk >= 0) {   // fine GC copies join the host frontier: release the GC frontier block
            for (int j = fgf.next; j < P; ++j) p2l[(u32)fgf.blk * P + j] = NONE;
            fgf.blk = -1; fgf.next = 0;
        }
        if (uni)   // close the age-cascade frontiers: their unwritten tails become ordinary garbage, not stuck OP
            for (int i = 0; i < MAXCC; ++i) if (ccf[i].blk >= 0) {
                for (int j = ccf[i].next; j < P; ++j) p2l[(u32)ccf[i].blk * P + j] = NONE;
                ccf[i].blk = -1; ccf[i].next = 0;
            }
    }

    // ---- fine_index=cuckoo2: the fine map as a statically sized 2-choice, 4-way bucketized cuckoo table of page
    // entries without kicks. Lookup / insert / delete touch at most the two candidate buckets (2 DRAM accesses);
    // when both are full, the cheapest IU among their 8 entries is folded back to its coarse chunk (evicted).
    bool ct_on = false; u64 ct_nb = 0; std::vector<u32> ct;
    static u64 ct_mix(u64 x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }
    u64 ct_b1(u64 lpn) const { return ct_mix(lpn) % ct_nb; }
    u64 ct_b2(u64 lpn) const { u64 b = ct_mix(lpn ^ 0x9E3779B97F4A7C15ULL) % ct_nb; return b == ct_b1(lpn) ? (b + 1) % ct_nb : b; }
    int ct_free_in(u64 b) const { for (int k = 0; k < 4; ++k) if (ct[b * 4 + k] == NONE) return k; return -1; }
    int ct_load(u64 b) const { int n = 0; for (int k = 0; k < 4; ++k) n += ct[b * 4 + k] != NONE; return n; }
    bool ct_room(u64 lpn) const { return ct_free_in(ct_b1(lpn)) >= 0 || ct_free_in(ct_b2(lpn)) >= 0; }
    int ct_depth(u64 lpn) const { u64 b = ct_b1(lpn); for (int k = 0; k < 4; ++k) if (ct[b * 4 + k] == (u32)lpn) return 1; return 2; }
    u64 fine_lookup_cost(u64 lpn) const { return ct_on ? (u64)ct_depth(lpn) : 1; }   // pool mode: one slot access
    u64 hb_cost(int32_t fi, u64 iu) const { u64 n = 0; u32 bm = fbitmap[fi]; while (bm) { int off = __builtin_ctz(bm); bm &= bm - 1; n += fine_lookup_cost(iu * U + off); } return n; }
    // cuckoo2 keeps no per-IU fragment bitmap (fine_idx/fbitmap are simulator bookkeeping, outside the RAM model):
    // the only per-IU state is the 4 B coarse entry = chunk index + one "has fragments" flag bit. Finding an IU's
    // fragment set (d, fragment ppns for fold/invalidate/recency) therefore looks up all U pages in the table:
    // present page = its depth (1-2 buckets), absent page = 2 buckets. 0 when the flag is clear (or not cuckoo2).
    u64 iu_scan_cost(u64 iu) const {
        int32_t fi = fine_idx[iu]; if (!ct_on || fi < 0) return 0;
        u64 n = 0; for (int k = 0; k < U; ++k) n += (fbitmap[fi] >> k & 1) ? (u64)ct_depth(iu * U + k) : 2;
        return n;
    }
    // one page's table lookup behind a set flag: 1-2 buckets if present, 2 if absent
    u64 flag_lookup_cost(int32_t fi, u64 lpn) const { return fi < 0 ? 0 : (fbitmap[fi] >> (lpn % U) & 1) ? (u64)ct_depth(lpn) : 2; }
    void ct_insert(u64 lpn) {
        u64 a = ct_b1(lpn), b = ct_b2(lpn); u64 t = ct_load(a) <= ct_load(b) ? a : b;
        int k = ct_free_in(t); if (k < 0) { t = (t == a) ? b : a; k = ct_free_in(t); }
        if (k < 0) DIE("cuckoo2: insert without room (lpn %llu)", (unsigned long long)lpn);
        ct[t * 4 + k] = (u32)lpn;
    }
    void ct_remove(u64 lpn) {
        for (u64 b : {ct_b1(lpn), ct_b2(lpn)}) for (int k = 0; k < 4; ++k) if (ct[b * 4 + k] == (u32)lpn) { ct[b * 4 + k] = NONE; return; }
        DIE("cuckoo2: remove of absent lpn %llu", (unsigned long long)lpn);
    }
    // make room for a NEW fine page of cur_iu before the host write enters the fine pool
    void ct_ensure(u64 lpn) {
        if (!ct_on) return;
        u64 cur_iu = lpn / U; int32_t fi = fine_idx[cur_iu];
        // membership: the coarse entry (charged by the caller) has the flag; flag set -> one table lookup
        if (fi >= 0 && (fbitmap[fi] >> (lpn % U) & 1)) { DA(ct_depth(lpn)); return; }   // overwrite of a resident page: no new entry
        if (fi >= 0) DA(2);                                              // flag set, page absent: both buckets were read
        for (int tries = 0; ; ++tries) {
            bool room = ct_room(lpn);
            if (tries > 0 || fi < 0) DA(ct_free_in(ct_b1(lpn)) >= 0 ? 1 : 2);   // candidate buckets read
            if (room) break;
            if (tries > 16) DIE("cuckoo2: cannot make room for lpn %llu", (unsigned long long)lpn);
            DCtx g(this, 3); u64 a0 = st.dacc[3];
            u64 best = ~0ull; int bd = -1; u64 bl = ~0ull; double bsc = -1;
            u64 seen[8]; int nseen = 0;
            for (u64 b : {ct_b1(lpn), ct_b2(lpn)}) for (int k = 0; k < 4; ++k) {
                u32 e = ct[b * 4 + k]; if (e == NONE) continue;
                u64 iu = e / U; if (iu == cur_iu) continue;
                int32_t f2 = fine_idx[iu]; if (f2 < 0) continue;
                int d = popc(fbitmap[f2]);                                   // most fragments first (cheapest fold), then coldest
                if (c.dram_stats) {   // candidate's fragment set (d + recency from its fragments' blocks): U lookups, once per IU
                    bool dup = false; for (int q = 0; q < nseen; ++q) dup |= seen[q] == iu;
                    if (!dup) { seen[nseen++] = iu; DA(iu_scan_cost(iu)); }
                }
                u64 rc = c.rec_block ? host_birth(f2) : flast[f2];
                if (c.evict_pol == 1) {        // fragments x age: cheap AND cold folds first
                    double sc = (double)d * (double)(fine_clock - std::min(rc, fine_clock) + 1);
                    if (sc > bsc) { bsc = sc; bd = d; bl = rc; best = iu; }
                } else if (c.evict_pol == 2) { // coldest first, then most fragments
                    if (rc < bl || (rc == bl && d > bd)) { bd = d; bl = rc; best = iu; }
                } else if (d > bd || (d == bd && rc < bl)) { bd = d; bl = rc; best = iu; }
            }
            if (best == ~0ull) DIE("cuckoo2: both buckets hold only the IU being written");
            st.n_evict_fold++;
            // eviction cost = candidate scan + the fold's own map work (coarse entry + fragment-set scan incl. removals);
            // a coarse GC nested in the fold's extent allocation is accounted as GC, not to the eviction
            u64 a = c.dram_stats ? st.dacc[3] - a0 + 1 + iu_scan_cost(best) : 0;
            fold(best, false);
            if (c.dram_stats) { st.d_evict++; st.d_evict_acc += a; if (a > st.d_evict_max) st.d_evict_max = a; }
        }
    }

    std::vector<uint8_t> bhost;   // fine block opened as the host frontier
    // rec_block: latest birth of a host-frontier fine block holding one of the IU's fragments (0 = none)
    u64 host_birth(int32_t fi) const {
        u64 m = 0; u32 bm = fbitmap[fi];
        while (bm) { int off = __builtin_ctz(bm); bm &= bm - 1; int b = blk_of(fslot[(u64)fi * U + off]); if (bhost[b] && fbirth[b] > m) m = fbirth[b]; }
        return m;
    }
    // bits of the coarse entry's chunk index: chunk ids 0..NP/U-1 plus one "no chunk" code (cuckoo2 adds a flag bit)
    int chunk_bits() const { u64 nc = NP / U; int b = 0; while ((1ull << b) <= nc) b++; return b; }
    double cuckoo_entry() const { return 4 + 4 + (!c.rec_block && (c.unfold != "off" || c.fine_policy == "cold" || c.fine_policy == "cost_cold") ? 4 : 0); }
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
        if (ct_on) { u32 bm = fbitmap[i]; while (bm) { int off = __builtin_ctz(bm); bm &= bm - 1; ct_remove(iu * U + off); } }   // DRAM cost: install_extent's scan
        else DA(popc(fbitmap[i]));
        fine_idx[iu] = -1;
        fpages -= popc(fbitmap[i]);
        fbitmap[i] = 0;
        fpool_free.push_back(i);
    }

    void erase_block(int b) {
        if (bvalid[b] != 0) DIE("erase block %d with valid=%d tag=%d", b, bvalid[b], btag[b]);
        if (c.mode == "proposed") {   // occupancy sample (diagnostics only)
            u64 fv = 0, cv = 0; for (int x = 0; x < NB; ++x) { if (btag[x] == B_FINE) fv += bvalid[x]; else if (btag[x] == B_COARSE) cv += bvalid[x]; }
            st.occ_n++; st.occ_fv += fv; st.occ_cv += cv; st.occ_fb += fine_blocks; st.occ_free += freelist.size(); st.occ_quota += fine_cap();
        }
        if (btag[b] == B_FINE) { fine_blocks--; st.erase_fine++; }
        else if (btag[b] == B_COARSE) { coarse_blocks--; st.erase_coarse++; }
        else DIE("erase free block");
        btag[b] = B_FREE;
        freelist.push_back(b);
        st.erases++;
    }

    void sample_fine_peak() {
        u64 fv = 0; for (int b = 0; b < NB; ++b) if (btag[b] == B_FINE) fv += bvalid[b];
        if (fv > st.fine_peak) st.fine_peak = fv;
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
            // Space guard: growing the fine pool must leave the coarse tier room for its resident
            // chunks plus the GC reserve; otherwise reclaim fine space first (quota is only a cap).
            auto coarse_starved = [&] {
                u64 fv = 0; for (int b = 0; b < NB; ++b) if (btag[b] == B_FINE) fv += bvalid[b];
                u64 resident = c.fine_elastic ? coarse_live() : L - fv;
                u64 slack = std::max((u64)P, resident / 32);   // min coarse OP ~3%: below this GC degenerates to full-block copies
                return (u64)(NB - fine_blocks - 1 - c.T) * (u64)P < resident + slack;
            };
            int stall = 0;
            while (fine_blocks >= fine_cap() || coarse_starved()) {
                if (fine_blocks < fine_cap()) st.n_space_capped++;
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
                // A fully valid best victim is direct evidence of over-commitment: coarse GC would
                // copy a whole block (P pages) for zero gain. Skip the copy and go shrink instead.
                if (c.fine_elastic && !in_shrink && !in_fine_reclaim && fine_blocks > fine_cap() && pick_victim(B_FINE) >= 0) {
                    in_shrink = true; fine_reclaim(); in_shrink = false; st.n_elastic_shrink++;
                    if (freelist.size() > before) { stall = 0; continue; }
                }
                bool useless = false;
                if (!is_pg() && !mixed && !ram_cap) {
                    int cv = pick_victim(B_COARSE);
                    useless = cv >= 0 && bvalid[cv] >= P;
                }
                if (!useless) {
                    if (is_pg()) page_gc();
                    else if (mixed) mixed_gc();
                    else if (ram_cap) {
                        int vf = pick_victim(B_FINE), vc = pick_victim(B_COARSE);
                        bool use_fine = vf >= 0 && (vc < 0 || bvalid[vf] <= bvalid[vc]);
                        if (use_fine) { in_cgc = true; fine_reclaim(); in_cgc = false; } else coarse_gc();
                    }
                    else if (c.gc_select == "cost" && !in_fine_reclaim && !in_shrink) {
                        // pick the cheaper tier: copy cost per freed block = v/(P-v); the fine pool
                        // gives back its slack whenever its victim is cheaper than the coarse one.
                        int vc = pick_victim(B_COARSE), vf = pick_victim(B_FINE);
                        bool use_fine = vf >= 0 && (vc < 0 || (c.gc_fine_bias == 1.0 ? bvalid[vf] < bvalid[vc] : bvalid[vf] < c.gc_fine_bias * bvalid[vc]));
                        if (use_fine) { in_shrink = true; fine_reclaim(); in_shrink = false; st.n_shrink_fold++; }
                        else coarse_gc();
                    }
                    else coarse_gc();
                }
                if (useless || freelist.size() <= before) {
                    ++stall;
                    {   // cheapest escape: a dead fine block frees space with zero copies and no recursion
                        int dead = -1;
                        for (int b2 = 0; b2 < NB; ++b2)
                            if (btag[b2] == B_FINE && !is_frontier(b2) && b2 != reclaim_victim && bvalid[b2] == 0) { dead = b2; break; }
                        if (dead >= 0) { erase_block(dead); stall = 0; continue; }
                    }
                    // Coarse tier over-committed: force-fold a fine victim block back into coarse and
                    // release the block, instead of dying. stall > 56 is the backstop for the ambiguous
                    // case (net-0 rounds without a fully valid victim).
                    // Not under in_cgc: the fold's coarse writes may legitimately run nested GC.
                    if ((useless || stall > 56) && !is_pg() && !mixed && !ram_cap && !in_shrink && !in_fine_reclaim && pick_victim(B_FINE) >= 0) {
                        in_shrink = true; fine_reclaim(true); in_shrink = false;
                        st.n_shrink_fold++;
                        if (freelist.size() > before) { stall = 0; continue; }
                    }
                    // A fine reclaim/shrink is in flight and will return a block when it completes:
                    // draw on the reserve instead of waiting for GC that cannot progress.
                    if ((useless || stall > 56) && (in_shrink || in_fine_reclaim) && freelist.size() > 1) break;   // keep the last block: a nested coarse GC needs it
                    if (stall > 64) DIE("GC makes no progress: coarse tier over-committed (free=%zu fine=%d coarse=%d quota=%d NB=%d). Try fine_extra=1 or smaller fine_quota.", freelist.size(), fine_blocks, coarse_blocks, fine_quota_blocks, NB);
                }
                else stall = 0;
            }
        }
        return pop_free(t);
    }

    // get one page slot on the fine frontier
    u32 fine_alloc_page(bool gc = false) {
        if (c.mode == "proposed" && !mixed && !ram_cap) { Frontier& g = (gc && c.gc_frontier) ? fgf : ff; if (g.blk < 0 || g.next == P) sample_fine_peak(); }
        bool sep = gc && c.gc_frontier && !(uni && c.uni_onefront);
        Frontier& f = mixed ? ((gc && c.gc_frontier) ? cgf : cf) : (sep ? fgf : ff);
        if (f.blk < 0 || f.next == P) { f.blk = alloc_block(mixed ? B_COARSE : B_FINE); f.next = 0; fbirth[f.blk] = fine_clock; bhost[f.blk] = (&f == &ff); }
        return (u32)f.blk * P + f.next++;
    }
    // get U consecutive slots on the coarse frontier
    u32 coarse_alloc_extent(bool gc = false, int cls = 0) {
        int ccls = std::min(cls, std::min(cc_eff(), MAXCC));
        Frontier* fp = (gc && c.gc_frontier) ? (ccls > 0 ? &ccf[ccls - 1] : &cgf) : &cf;
        // Cascade fallback: opening an extra class frontier must not drain the GC reserve --
        // reroute to the shared GC frontier (or any open class frontier) instead of dying.
        if (ccls > 0 && (fp->blk < 0 || P - fp->next < U) && freelist.size() <= 1) {
            if (cgf.blk >= 0 && P - cgf.next >= U) { fp = &cgf; }
            else { for (int i = 0; i < MAXCC; ++i) if (ccf[i].blk >= 0 && P - ccf[i].next >= U) { fp = &ccf[i]; break; } }
        } else if (fp == &cgf && (cgf.blk < 0 || P - cgf.next < U) && in_cgc && freelist.empty()) {
            // ... and symmetrically, as a last resort: inside coarse GC with no free block left, a full shared
            // GC frontier spills into an open class frontier instead of popping an empty free list.
            for (int i = 0; i < MAXCC; ++i) if (ccf[i].blk >= 0 && P - ccf[i].next >= U) { fp = &ccf[i]; break; }
        }
        Frontier& f = *fp;
        if (f.blk >= 0 && P - f.next < U) { for (int i = f.next; i < P; ++i) p2l[(u32)f.blk * P + i] = NONE; f.next = P; }  // mixed: chunk must not straddle
        if (f.blk < 0 || f.next == P) { f.blk = alloc_block(B_COARSE); f.next = 0; bccls[f.blk] = (int8_t)(&f == &cf ? 0 : (&f == &cgf ? 1 : ccls)); }
        u32 ppn = (u32)f.blk * P + f.next; f.next += U;
        return ppn;
    }
    bool is_frontier(int b) const {
        if (b == ff.blk || b == cf.blk || b == fgf.blk || b == cgf.blk) return true;
        for (int i = 0; i < MAXCC; ++i) if (b == ccf[i].blk) return true;
        if (is_sb()) for (int i = 0; i < 6; ++i) if (b == sbf[i].blk) return true;
        return false;
    }
    u32 sb_alloc(int cls) {
        Frontier& f = sbf[cls];
        if (f.blk < 0 || f.next == P) { f.blk = alloc_block(B_FINE); f.next = 0; sb_birth[f.blk] = sb_now; sb_class[f.blk] = (int8_t)cls; }
        return (u32)f.blk * P + f.next++;
    }

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
        if (c.dram_stats && src == S_HOST && !ct_on) DA((fbitmap[fi] & bit) ? fine_lookup_cost(lpn) : 1);   // overwrite: find entry; new: coarse entry (cuckoo2: ct_ensure)
        if (fbitmap[fi] & bit) {
            u32 old = fslot[(u64)fi * U + off];
            bvalid[blk_of(old)]--;
            if (src == S_HOST) { st.frag_freed++; st.fine_host_ow++; if (c.adapt) adapt_tick(true); }
        } else {
            if (src == S_HOST) { st.fine_host_new++; if (c.adapt) adapt_tick(false); }
            if (ct_on) ct_insert(lpn);
            if (coarse_map[iu] != NONE) bvalid[blk_of(coarse_map[iu])]--;  // superseded in extent
            int od = popc(fbitmap[fi]);
            fbitmap[fi] |= bit; fpages++;
            track_d(iu, od, od + 1);
            if (popc(fbitmap[fi]) == U && coarse_map[iu] != NONE) { coarse_map[iu] = NONE; n_chunks--; st.n_extent_drop++; }  // extent fully dead
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
    // cost-based eager test as a function of u alone; it is monotone in u (true for small u), so the request's own
    // n pages (u >= n) and a full IU (u <= U) bound it: only between the two does the fragment set matter
    bool eager_test_u(int n, int u) const {
        double p = (st.frag_folded + st.frag_freed) ? (double)st.frag_folded / (st.frag_folded + st.frag_freed) : 0.5;
        return (double)U / n < 1.0 + p * (double)U / std::max(1, u);
    }
    bool eager_ambiguous(int n) const { return eager_test_u(n, n) && !eager_test_u(n, U); }
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
        // Age cascade: only untouched survivors of coarse GC advance a class; host writes,
        // refolds and folds carry fresh content and restart at class 0/1.
        int cls = 0;
        if (cc_eff() > 0 && src != S_HOST) cls = (src == S_CGC_CLEAN) ? cgc_cls + 1 : 1;
        u32 ppn = coarse_alloc_extent(src != S_HOST, cls);
        install_extent(iu, ppn, src, nprog_src, src2);
    }
    void install_extent(u64 iu, u32 ppn, Src src, u64 nprog_src = 0, Src src2 = S_HOST) {
        if (ct_on) DA(1 + iu_scan_cost(iu));                         // coarse entry (flag) + fragment-set scan / removal
        else DA(2);                                                  // IU bitmap + coarse entry
        for (int k = 0; k < U; ++k) p2l[ppn + k] = (u32)(iu * U + k);
        bvalid[blk_of(ppn)] += U;
        if (nprog_src) { st.prog[src] += nprog_src; st.prog[src2] += U - nprog_src; }
        else st.prog[src] += U;
        if (src == S_HOST) st.host_coarse += U;
        invalidate_iu(iu);
        if (coarse_map[iu] == NONE) n_chunks++;
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
        DCtx g(this, 3);
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
        return fine_blocks < fine_quota_blocks && (int)freelist.size() >= c.T;  // GC runs at free==T, so >= (victim erase restores one)
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
            { int od = popc(fbitmap[fi]); fbitmap[fi] |= (1u << off); fpages++; track_d(iu, od, od + 1); } fslot[(u64)fi * U + off] = np;
            bvalid[blk_of(old)]--;                    // that chunk page is now superseded
        }
        if (popc(fbitmap[fi]) == U) { if (coarse_map[iu] != NONE) n_chunks--; coarse_map[iu] = NONE; st.n_extent_drop++; }
        in_fine_reclaim = saved;
    }

    // ------------------------------------------------------------------ reclaim
    // FineGC(block): copy valid 4K pages to fine frontier, erase
    void fine_gc_block(int b) {
        DCtx g(this, 2);
        int rv_save = reclaim_victim; reclaim_victim = b;
        int v0 = bvalid[b];
        st.n_finegc++; st.finegc_valid_sum += v0;
        u32 base = (u32)b * P;
        for (int s = 0; s < P && bvalid[b] > 0; ++s) {
            u32 ppn = base + s;
            u32 lpn = p2l[ppn];
            if (lpn == NONE) continue;
            u64 iu = lpn / U; int off = (int)(lpn % U);
            int32_t fi = fine_idx[iu];
            if (c.dram_stats) DA(1 + (ct_on ? flag_lookup_cost(fi, lpn) : (fi >= 0 && (fbitmap[fi] & (1u << off))) ? fine_lookup_cost(lpn) : 0));
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
        reclaim_victim = rv_save;
    }

    // fold every IU that has valid pages in block b (block-wise fold), then erase
    void fold_block(int b) {
        DCtx g(this, 2);
        int rv_save = reclaim_victim; reclaim_victim = b;
        st.n_blockfold++;
        u32 base = (u32)b * P;
        for (int s = 0; s < P && bvalid[b] > 0; ++s) {
            u32 ppn = base + s;
            u32 lpn = p2l[ppn];
            if (lpn == NONE) continue;
            u64 iu = lpn / U; int off = (int)(lpn % U);
            int32_t fi = fine_idx[iu];
            if (c.dram_stats) DA(1 + (ct_on ? flag_lookup_cost(fi, lpn) : (fi >= 0 && (fbitmap[fi] & (1u << off))) ? fine_lookup_cost(lpn) : 0));
            if (fi < 0 || !(fbitmap[fi] & (1u << off)) || fslot[(u64)fi * U + off] != ppn) continue;
            fold(iu, false);  // invalidates all fragments of iu incl. this one
        }
        if (bvalid[b] != 0) DIE("fold_block left valid=%d", bvalid[b]);
        erase_block(b);
        reclaim_victim = rv_save;
    }


    // greedy: per valid page in victim, fold its IU if the IU has >= fold_d fragments (fold retires all of them,
    // in this and other fine blocks), otherwise copy the page within the fine tier. Then erase.
    void greedy_reclaim_block(int b) {
        DCtx g(this, 2);
        int rv_save = reclaim_victim; reclaim_victim = b;
        st.n_greedy++;
        u32 base = (u32)b * P;
        for (int s0 = 0; s0 < P && bvalid[b] > 0; ++s0) {
            u32 ppn = base + s0;
            u32 lpn = p2l[ppn];
            if (lpn == NONE) continue;
            u64 iu = lpn / U; int off = (int)(lpn % U);
            int32_t fi = fine_idx[iu];
            if (c.dram_stats) DA(1 + (ct_on ? flag_lookup_cost(fi, lpn) : (fi >= 0 && (fbitmap[fi] & (1u << off))) ? fine_lookup_cost(lpn) : 0));
            if (fi < 0 || !(fbitmap[fi] & (1u << off)) || fslot[(u64)fi * U + off] != ppn) continue;
            int d = popc(fbitmap[fi]);
            if (s0 == 0 || st.vblk_age_n == 0 || last_vblk != b) {   // once per victim block
                last_vblk = b;
                double bt = (double)(fine_clock - fbirth[b]) / ((double)fine_quota_blocks * P);
                st.vblk_age_sum += fine_clock - fbirth[b]; st.vblk_age_n++;
                int bk = bt < 0.25 ? 0 : bt < 0.5 ? 1 : bt < 1 ? 2 : bt < 2 ? 3 : bt < 4 ? 4 : 5;
                st.vblk_age_hist[bk]++;
            }
            double turns = (double)(fine_clock - flast[fi]) / ((double)fine_quota_blocks * P);
            { int bk = turns < 0.125 ? 0 : turns < 0.25 ? 1 : turns < 0.5 ? 2 : turns < 1 ? 3 : turns < 2 ? 4 : turns < 4 ? 5 : 6;
              st.age_hist[bk]++; if ((double)popc(fbitmap[fi]) * victim_v > (double)U * (P - victim_v)) st.age_hist_fold[bk]++; }
            // Parameter-free (fold_tau < 0): cold iff the IU was never rewritten while this block lived.
            // The block's own residence time is the pool's natural turnover clock, so no constant is needed.
            if (c.dram_stats && ct_on) DA(iu_scan_cost(iu));                 // d (+ rec_block recency): fragment-set scan
            else if (c.dram_stats && c.rec_block && c.fold_tau < 0) DA(hb_cost(fi, iu));
            bool cold = c.fold_tau < 0 ? (c.rec_block ? host_birth(fi) <= fbirth[b] : flast[fi] <= fbirth[b])
                                       : (double)(fine_clock - flast[fi]) >= c.fold_tau * (double)fine_quota_blocks * P;
            bool do_fold;
            if (c.fine_policy == "cost" || c.fine_policy == "cost_cold") {
                // per-slot cost: fold = U/d, copy-path = v/(P-v)  ->  fold iff d*v > U*(P-v)
                bool cheaper = (double)d * victim_v > (double)U * (P - victim_v);
                do_fold = c.fine_policy == "cost" ? cheaper : (cheaper && cold);
            } else do_fold = c.fine_policy == "cold" ? (fold_allowed && cold && d >= c.fold_d) : (d >= c.fold_d);
            if (do_fold) { st.greedy_fold_pages += d; st.n_greedy_fold++; fold(iu, false); continue; }
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
        reclaim_victim = rv_save;
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
        st.greedy_v_sum += v; if (in_shrink) { st.n_shrink_reclaim++; st.shrink_v_sum += v; }
        if (c.fine_policy == "greedy" || c.fine_policy == "cold" || c.fine_policy == "cost" || c.fine_policy == "cost_cold") { fold_allowed = (double)v / P >= c.fold_gate; victim_v = v; greedy_reclaim_block(b); in_fine_reclaim = false; return; }
        double k = (double)ius.size();
        // cost (writes) per freed fine slot
        double cost_gc = v >= P ? 1e18 : (double)v / (double)(P - v);        // copy v fine pages, net gain P-v
        double cost_fold = (double)U * k / (double)(P + extra);              // write 16k coarse pages, gain P + fragments freed elsewhere
        if (cost_fold < cost_gc) fold_block(b);
        else fine_gc_block(b);
        in_fine_reclaim = false;
    }

    int pick_victim_cb() {   // cost-benefit (Rosenblum): max garbage/valid * sqrt(age), age from alloc sequence
        int best = -1; double bs = -1;
        for (int b = 0; b < NB; ++b) {
            if (btag[b] != B_COARSE || is_frontier(b)) continue;
            double score = bvalid[b] == 0 ? 1e308
                : (double)(P - bvalid[b]) / (double)bvalid[b] * std::sqrt((double)(alloc_seq - bseq[b]));
            if (score > bs) { bs = score; best = b; }
        }
        return best;
    }
    // CoarseGC(block): victim = min valid coarse block (or cost-benefit with cgc_vict=cb)
    void coarse_gc() {
        DCtx g(this, 2);
        in_cgc = true;
        int b = (c.cgc_vict == "cb" && !uni) ? pick_victim_cb() : pick_victim(B_COARSE);
        if (b < 0) DIE("no coarse victim (free=%zu fine=%d coarse=%d)", freelist.size(), fine_blocks, coarse_blocks);
        st.n_cgc++; st.cgc_valid_sum += bvalid[b];
        cgc_cls = bccls[b];
        u32 base = (u32)b * P;
        for (int e = 0; e < P / U && bvalid[b] > 0; ++e) {
            u32 ppn = base + e * U;
            u32 lpn = p2l[ppn];
            if (lpn == NONE) continue;
            u64 iu = lpn / U;
            if (coarse_map[iu] != ppn) {
                DA(1 + (coarse_map[iu] == NONE && fine_idx[iu] >= 0 && popc(fbitmap[fine_idx[iu]]) == U ? iu_scan_cost(iu) : 0));
                st.cgc_dead_extents++; continue;
            }
            int32_t fi = fine_idx[iu];
            if (fi >= 0 && fbitmap[fi]) {
                bool un = c.unfold == "any" || (c.unfold == "hot" && iu_hot(iu));
                if (un && can_unfold(U - popc(fbitmap[fi]))) unfold(iu); else fold(iu, true);
            } else if (c.unfold == "any" && can_unfold(U)) {
                // coarse pool need not own every IU: evict this (fragment-less) IU wholly into the fine pool.
                fine_get_or_create(iu);
                unfold(iu);
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
        u32 ppn;
        if (is_sb()) {
            int cls;
            if (src == S_HOST) {
                // class 0 iff the previous copy's lifespan is short (< avg); GC appends never tick time
                cls = (sb_wtime[lpn] != ~0ull && (double)(sb_now - sb_wtime[lpn]) < sb_avg) ? 0 : 1;
                sb_wtime[lpn] = sb_now; sb_now++;
            } else {
                if (sb_class[sb_gc_victim] == 0) cls = 2;
                else { double age = (double)(sb_now - sb_wtime[lpn]);
                       cls = age < 4 * sb_avg ? 3 : (age < 16 * sb_avg ? 4 : 5); }
            }
            ppn = sb_alloc(cls);
        } else ppn = fine_alloc_page(src != S_HOST);
        p2l[ppn] = (u32)lpn;
        bvalid[blk_of(ppn)]++;
        st.prog[src]++;
        u32 old = pmap[lpn];
        if (c.mode == "page4k" || is_sb()) DA(1);                    // L2P entry (read old + write new)
        else if (src != S_HOST) DA(1);                                // dftl / leaftl GC: page validity check
        if (old != NONE) bvalid[blk_of(old)]--;
        pmap[lpn] = ppn;
        if (src == S_HOST) cmt_update(lpn); else cmt_gc_update(lpn);
    }
    int sb_gc_victim = -1;
    int sb_pick_victim() {   // reference default: cost-benefit, score = garbage/valid * sqrt(age), max wins
        int best = -1; double bs = -1;
        for (int b = 0; b < NB; ++b) {
            if (btag[b] != B_FINE || is_frontier(b)) continue;
            double score = bvalid[b] == 0 ? 1e308
                : (double)(P - bvalid[b]) / (double)bvalid[b] * std::sqrt((double)(sb_now - sb_birth[b]));
            if (score > bs) { bs = score; best = b; }
        }
        return best;
    }
    void page_gc() {
        DCtx g(this, 2);
        in_cgc = true;
        int b = is_sb() ? sb_pick_victim() : pick_victim(B_FINE);
        if (b < 0) DIE("no page4k victim");
        if (is_sb()) {
            sb_gc_victim = b;
            if (sb_class[b] == 0) {   // mAvgLifespan = mean age of class-0 segments at collection, every 16
                sb_lifesum += sb_now - sb_birth[b];
                if (++sb_ncollect == 16) { sb_avg = (double)sb_lifesum / 16.0; sb_lifesum = 0; sb_ncollect = 0; }
            }
        }
        st.n_pgc++; st.pgc_valid_sum += bvalid[b];
        u32 base = (u32)b * P;
        if (is_lf()) {   // valid pages found via OOB/validity bitmap, rewritten in LPN order and relearned
            std::vector<u32> lp;
            for (int s = 0; s < P; ++s) { u32 ppn = base + s; u32 lpn = p2l[ppn]; if (lpn != NONE && pmap[lpn] == ppn) lp.push_back(lpn); }
            if (c.leaftl_gcsort) std::sort(lp.begin(), lp.end());
            std::vector<std::pair<u64,u64>> pairs;
            for (u32 lpn : lp) { st.read_gc_copy++; page_write(lpn, S_PGC); pairs.push_back({lpn, pmap[lpn]}); }
            if (bvalid[b] != 0) DIE("page_gc left valid=%d", bvalid[b]);
            erase_block(b);
            in_cgc = false;
            std::vector<std::pair<u64,u64>> cur;
            for (auto& p : pairs) if (pmap[p.first] == p.second) cur.push_back(p);
            if (!c.leaftl_gcsort) std::sort(cur.begin(), cur.end());
            lf_insert(cur);
            return;
        }
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
        DCtx dg(this, 1);
        st.host_pages += n; req_id++;
        u64 end = lpn + n;
        if (is_pg()) {
            if (is_lf()) { for (u64 p = lpn; p < end; ++p) lf_host(p); }
            else for (u64 p = lpn; p < end; ++p) page_write(p, S_HOST);
        } else if (c.mode == "proposed" || c.mode == "mixed") {
            if (uni) st.uni_host += n;
            u64 cur = lpn;
            while (cur < end) {
                if (cur % U == 0 && cur + U <= end) {
                    u64 iu = cur / U;
                    if (c.fine_full && fine_idx[iu] >= 0 && fine_blocks < fine_cap()) {
                        // hot (fine-resident) IU fully overwritten: keep it page-mapped instead of re-chunking
                        for (int i = 0; i < U; ++i) { u64 a0 = st.dacc[1]; DA(1); ct_ensure(cur + i); fine_write(cur + i, S_HOST); d_wp(a0); }
                    } else { u64 a0 = st.dacc[1]; DA(1); coarse_write(iu, S_HOST); d_wiu(a0, U); }
                    cur += U;
                }
                else {
                    u64 iu = cur / U;
                    u64 e = std::min(end, (iu + 1) * U);          // this request's part of the IU
                    int n = (int)(e - cur);
                    bool keep_fine = c.fine_full && fine_idx[iu] >= 0 && fine_blocks < fine_cap();
                    u64 a_pre = st.dacc[1];
                    if (c.dram_stats && ct_on && !keep_fine && fine_idx[iu] >= 0 &&
                        ((c.eager_u < 0 && c.eager_u > -10 && eager_ambiguous(n)) || (c.eager_u > 0 && n <= c.eager_u && c.eager_u < U)))
                        DA(iu_scan_cost(iu));   // u = |request | fragments| matters: scan the fragment set
                    if (!keep_fine && c.eager_u != 0 && eager_worth(iu, cur, n)) { u64 a0 = a_pre; DA(1); eager_write(iu, cur, n); d_wiu(a0, n); cur = e; }
                    else { bool first = true; while (cur < e) { u64 a0 = first ? a_pre : st.dacc[1]; first = false; DA(1); ct_ensure(cur); fine_write(cur, S_HOST); d_wp(a0); cur++; } }
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

    void d_wp(u64 a0) { if (!c.dram_stats) return; u64 a = st.dacc[1] - a0; st.d_wp_ops++; if (a > st.d_wp_max) st.d_wp_max = a; }
    void d_wiu(u64 a0, u64 pages) { if (!c.dram_stats) return; u64 a = st.dacc[1] - a0; st.d_wiu_ops++; st.d_wiu_pages += pages; if (a > st.d_wiu_max) st.d_wiu_max = a; }
    // host read of one page through the mapping (dram_stats only)
    void read_lookup(u64 lpn) {
        DCtx dg(this, 0);
        u64 a0 = st.dacc[0], f0 = st.dfr[0];
        if (c.mode == "page4k" || is_sb()) { DA(1); if (pmap[lpn] == NONE) st.d_rd_unmapped++; }
        else if (c.mode == "dftl") { cmt_read(lpn); }
        else if (is_lf()) {
            if (lf_stage_set.count((u32)lpn)) DA(1);                 // still in the sort batch (DRAM)
            else if (pmap[lpn] == NONE) { DA(1); st.d_rd_unmapped++; }
            else {
                lf_touch_lookup(lpn); lf_enforce();
                bool acc = true; u64 pr = lf_predict(lpn, &acc);
                if (pr != pmap[lpn]) { st.d_lf_mispredict++; MR(); }   // approximate segment: read the wrong page first
            }
        } else if (c.mode == "proposed") {
            u64 iu = lpn / U; int off = (int)(lpn % U); int32_t fi = fine_idx[iu];
            if (ct_on) {   // coarse entry (chunk + flag); flag set -> one table lookup (<= 2 buckets); never a flash map read
                DA(1 + flag_lookup_cost(fi, lpn));
                if (!(fi >= 0 && (fbitmap[fi] & (1u << off))) && coarse_map[iu] == NONE) st.d_rd_unmapped++;
            } else {
            DA(1);                                                     // IU bitmap
            if (fi >= 0 && (fbitmap[fi] & (1u << off))) DA(fine_lookup_cost(lpn));
            else { DA(1); if (coarse_map[iu] == NONE) st.d_rd_unmapped++; }
            }
        } else if (c.mode == "iu_rmw") { DA(1); }
        u64 a = st.dacc[0] - a0, fr = st.dfr[0] - f0;
        st.d_rd_ops++; if (a > st.d_rd_max) st.d_rd_max = a; st.d_rd_hist[std::min<u64>(a, 15)]++;
        if (fr > st.d_rd_fr_max) st.d_rd_fr_max = fr;
    }

    void ts_header() { fprintf(ts, "host_pages,waf_cum,f,fine_blocks,coarse_blocks,free,fv,ct_used,uni,evict,gfold,forced,refold,cgc,cgc_clean_ext,finegc_copy,fold_prog,clean_prog,refold_prog,finegc_prog,host_prog\n"); }
    void ts_sample() {
        u64 tp = st.total_prog();
        u64 fv = 0; for (int b = 0; b < NB; ++b) if (btag[b] == B_FINE) fv += bvalid[b];
        u64 used = 0; if (ct_on) for (u32 e : ct) used += e != NONE;
        fprintf(ts, "%llu,%.5f,%.5f,%d,%d,%zu,%llu,%llu,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", (unsigned long long)st.host_pages,
                st.host_pages ? (double)tp / st.host_pages : 0.0,
                (double)fine_blocks / NB, fine_blocks, coarse_blocks, freelist.size(),
                (unsigned long long)fv, (unsigned long long)used, (int)uni, (unsigned long long)st.n_evict_fold, (unsigned long long)st.n_greedy_fold,
                (unsigned long long)st.n_forced_fold, (unsigned long long)st.n_refold, (unsigned long long)st.n_cgc, (unsigned long long)st.cgc_clean_extents,
                (unsigned long long)st.greedy_copy_pages, (unsigned long long)st.prog[S_FOLD], (unsigned long long)st.prog[S_CGC_CLEAN], (unsigned long long)st.prog[S_CGC_REFOLD],
                (unsigned long long)st.prog[S_FINEGC], (unsigned long long)st.prog[S_HOST]);
    }

    void verify() {
        if (is_lf()) lf_flush_stage();
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
        if (!is_pg() && fpages != fine_valid) DIE("verify: fine page counter %llu != %llu", (unsigned long long)fpages, (unsigned long long)fine_valid);
        if (!is_pg()) { u64 nc = 0; for (u64 i = 0; i < NIU; ++i) nc += coarse_map[i] != NONE; if (nc != n_chunks) DIE("verify: chunk counter %llu != %llu", (unsigned long long)n_chunks, (unsigned long long)nc); }
        if (ct_on) {   // table <-> fine pool consistency: every fine page has exactly one entry, nothing else does
            u64 used = 0; for (u32 e : ct) if (e != NONE) { used++; int32_t f = fine_idx[e / U]; if (f < 0 || !(fbitmap[f] >> (e % U) & 1)) DIE("verify: cuckoo2 stale entry %u", e); }
            if (used != fine_valid) DIE("verify: cuckoo2 has %llu entries, fine pool %llu pages", (unsigned long long)used, (unsigned long long)fine_valid);
        }
        if (is_lf()) lf_verify();
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
        printf("  \"events\": {\"finegc\": %llu, \"blockfold\": %llu, \"fold\": %llu, \"refold\": %llu, \"cgc\": %llu, \"pgc\": %llu, \"cgc_dead_ext\": %llu, \"cgc_clean_ext\": %llu, \"extent_drop\": %llu, \"wasted_slots\": %llu, \"greedy\": %llu, \"greedy_fold_pages\": %llu, \"greedy_copy_pages\": %llu, \"forced_fold\": %llu, \"eager\": %llu, \"eager_fill\": %llu, \"unfold\": %llu, \"unfold_pages\": %llu, \"budget_fold\": %llu, \"gc_fold\": %llu, \"shrink_fold\": %llu, \"space_capped\": %llu},\n",
               (unsigned long long)st.n_finegc, (unsigned long long)st.n_blockfold, (unsigned long long)st.n_fold, (unsigned long long)st.n_refold,
               (unsigned long long)st.n_cgc, (unsigned long long)st.n_pgc, (unsigned long long)st.cgc_dead_extents, (unsigned long long)st.cgc_clean_extents, (unsigned long long)st.n_extent_drop, (unsigned long long)st.wasted_slots, (unsigned long long)st.n_greedy, (unsigned long long)st.greedy_fold_pages, (unsigned long long)st.greedy_copy_pages, (unsigned long long)st.n_forced_fold, (unsigned long long)st.n_eager, (unsigned long long)st.eager_fill, (unsigned long long)st.n_unfold, (unsigned long long)st.unfold_pages, (unsigned long long)st.n_budget_fold, (unsigned long long)st.n_gcfold, (unsigned long long)st.n_shrink_fold, (unsigned long long)st.n_space_capped);
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
        { u64 fine_ius = 0, fine_pages = 0, fdh[33] = {0}; for (u64 i = 0; i < NIU; ++i) if (fine_idx[i] >= 0) { int dd = popc(fbitmap[fine_idx[i]]); fine_ius++; fine_pages += dd; fdh[dd]++; }
          printf("  \"fine_d_hist_end\": ["); for (int dd = 0; dd <= U; ++dd) printf("%s%llu", dd ? "," : "", (unsigned long long)fdh[dd]); printf("],\n");
          printf("  \"fine_ius_end\": %llu, \"fine_pages_end\": %llu,\n", (unsigned long long)fine_ius, (unsigned long long)fine_pages);
          // RAM model (measured end state). PPN = 4B. Hash-managed maps: (key+value)/0.7.
          double mem = 0, a = 0, b2 = 0, c2 = 0;
          if (c.mode == "page4k") { a = (double)L * 4; mem = a; }
          // SepBIT also needs a per-LPN last-user-write timestamp for the GC-time age test
          // (reference keeps 8 B in Metadata; 4 B suffices) plus per-block birth/class.
          else if (c.mode == "sepbit") { a = (double)L * 4; b2 = (double)L * 4; c2 = (double)NB * 9; mem = a + b2 + c2; }
          else if (c.mode == "dftl") { a = hash_bytes((double)cmt.size(), 8); b2 = (double)(L / TP_ENTRIES) * 4; mem = a + b2; }
          else if (c.mode == "iu_rmw") { a = (double)NIU * 4; mem = a; }
          else if (is_lf()) { a = lf_cached; b2 = (double)((L + LF_FRAME - 1) / LF_FRAME) * 4; mem = a + b2; }   // resident frames + GTD
          else if (ct_on) { a = (double)NIU * 4; c2 = (double)ct_nb * 4 * cuckoo_entry(); mem = a + c2; }   // coarse entry 4 B incl. the fragment flag bit
          else if (c.fine_index == "cuckoo") { a = (double)NIU * 4; c2 = (double)fine_pages * cuckoo_entry() / 0.95; mem = a + c2; }
          else { a = (double)NIU * 4; b2 = hash_bytes((double)fine_ius, 4 + 2 + 4 + ((c.unfold != "off" || c.fine_policy == "cold" || c.fine_policy == "cost_cold") ? 4 : 0)); c2 = (double)fine_pages * 4; mem = a + b2 + c2; }
          printf("  \"mem\": {\"total_bytes\": %.0f, \"total_mib\": %.2f, \"primary_map\": %.0f, \"hash_index\": %.0f, \"slots\": %.0f},\n", mem, mem / (1 << 20), a, b2, c2); }
        { printf("  \"vblk_age_mean_turnovers\": %.4f, \"vblk_age_hist\": [",
                 st.vblk_age_n ? (double)st.vblk_age_sum / st.vblk_age_n / ((double)fine_quota_blocks * P) : 0.0);
          for (int i=0;i<6;++i) printf("%s%llu", i?",":"", (unsigned long long)st.vblk_age_hist[i]); printf("],\n"); }
        { printf("  \"age_turnovers\": ["); for (int i=0;i<7;++i) printf("%s%llu", i?",":"", (unsigned long long)st.age_hist[i]);
          printf("], \"age_turnovers_costfold\": ["); for (int i=0;i<7;++i) printf("%s%llu", i?",":"", (unsigned long long)st.age_hist_fold[i]); printf("],\n"); }
        printf("  \"host_fine\": %llu, \"host_coarse\": %llu,\n", (unsigned long long)st.host_fine, (unsigned long long)st.host_coarse);
        printf("  \"fine_host_new\": %llu, \"fine_host_ow\": %llu, \"fine_pages_peak\": %llu,\n", (unsigned long long)st.fine_host_new, (unsigned long long)st.fine_host_ow, (unsigned long long)st.fine_peak);
        if (ct_on) { u64 used = 0; for (u32 e : ct) used += e != NONE;
            printf("  \"cuckoo2\": {\"entries\": %llu, \"used_end\": %llu, \"evict_folds\": %llu, \"coarse_entry_chunk_bits\": %d, \"coarse_entry_flag_bits\": 1},\n",
                   (unsigned long long)ct.size(), (unsigned long long)used, (unsigned long long)st.n_evict_fold, chunk_bits()); }
        if (c.mode == "proposed" && c.fine_index == "cuckoo")
            printf("  \"mem_cuckoo_peak\": %.0f,\n", (double)NIU * 4 + (double)st.fine_peak * cuckoo_entry() / 0.95);
        if (c.adapt) printf("  \"adapt\": {\"switches\": %llu, \"uni_host_frac\": %.4f, \"quota_base\": %d, \"quota_uni\": %d, \"uni_at_end\": %d},\n",
                            (unsigned long long)n_mode_switch, st.host_pages ? (double)st.uni_host / st.host_pages : 0.0, fine_quota_base, fine_quota_uni, (int)uni);
        if (is_lf()) { double full = 0; u64 nseg = 0, nacc = 0, ncons = 0; for (auto& kv : lframes) { full += kv.second.mem; for (auto& g : kv.second.segs) { nseg++; nacc += g.accurate; ncons += g.consec; } }
          printf("  \"leaftl\": {\"full_table_bytes\": %.0f, \"segments\": %llu, \"accurate\": %llu, \"consecutive\": %llu, \"frames\": %zu, \"lookups\": %llu, \"gamma\": %g, \"batch\": %d},\n",
                 full, (unsigned long long)nseg, (unsigned long long)nacc, (unsigned long long)ncons, lframes.size(), (unsigned long long)lf_lookups, c.leaftl_gamma, c.leaftl_batch); }
        if (c.mode == "proposed") {
            double on = st.occ_n ? (double)st.occ_n : 1;
            printf("  \"diag\": {\"fine_victim_valid_frac\": %.4f, \"greedy_reclaims\": %llu, \"greedy_folds\": %llu, \"evict_folds\": %llu, \"shrink_reclaims\": %llu, \"shrink_victim_valid_frac\": %.4f, "
                   "\"cgc_victim_valid_frac\": %.4f, \"cgc_prog_per_victim_frac\": %.4f, \"mode_switches\": %llu, \"uni_host_frac\": %.4f, "
                   "\"occ_fine_valid_frac_L\": %.4f, \"occ_coarse_valid_frac_L\": %.4f, \"occ_fine_blocks\": %.2f, \"occ_quota\": %.2f, \"occ_free\": %.2f, \"occ_fine_util\": %.4f, \"elastic_shrinks\": %llu, \"chunks_end_frac_L\": %.4f},\n",
                   st.n_greedy ? (double)st.greedy_v_sum / st.n_greedy / P : 0.0, (unsigned long long)st.n_greedy, (unsigned long long)st.n_greedy_fold,
                   (unsigned long long)st.n_evict_fold, (unsigned long long)st.n_shrink_reclaim, st.n_shrink_reclaim ? (double)st.shrink_v_sum / st.n_shrink_reclaim / P : 0.0,
                   st.n_cgc ? (double)st.cgc_valid_sum / st.n_cgc / P : 0.0, st.n_cgc ? (double)(st.prog[S_CGC_CLEAN] + st.prog[S_CGC_REFOLD]) / st.n_cgc / P : 0.0,
                   (unsigned long long)n_mode_switch, st.host_pages ? (double)st.uni_host / st.host_pages : 0.0,
                   st.occ_fv / on / L, st.occ_cv / on / L, st.occ_fb / on, st.occ_quota / on, st.occ_free / on,
                   st.occ_fb > 0 ? st.occ_fv / (st.occ_fb * P) : 0.0, (unsigned long long)st.n_elastic_shrink, (double)n_chunks * U / L);
        }
        if (c.dram_stats) {
            u64 gcp = st.prog[S_FINEGC] + st.prog[S_CGC_CLEAN] + st.prog[S_PGC];
            u64 fdp = st.prog[S_FOLD] + st.prog[S_CGC_REFOLD] + st.prog[S_ZEROFILL];
            auto R = [](u64 a, u64 b) { return b ? (double)a / (double)b : 0.0; };
            printf("  \"dram\": {\"host_read_pages\": %llu, \"acc_per_host_read\": %.4f, \"max_acc_host_read\": %llu, \"flash_map_reads_per_host_read\": %.5f, \"max_flash_map_reads_host_read\": %llu, "
                   "\"read_from_buffer\": %llu, \"read_unmapped\": %llu, \"read_acc_hist\": [",
                   (unsigned long long)st.d_rd_ops, R(st.dacc[0], st.d_rd_ops), (unsigned long long)st.d_rd_max, R(st.dfr[0], st.d_rd_ops), (unsigned long long)st.d_rd_fr_max,
                   (unsigned long long)st.d_rd_buf, (unsigned long long)st.d_rd_unmapped);
            for (int i = 0; i < 16; ++i) printf("%s%llu", i ? "," : "", (unsigned long long)st.d_rd_hist[i]);
            printf("], \"host_write_pages\": %llu, \"acc_per_host_write\": %.4f, \"flash_map_reads_per_host_write\": %.5f, "
                   "\"fine_page_ops\": %llu, \"max_acc_fine_page_write\": %llu, \"iu_write_ops\": %llu, \"iu_write_pages\": %llu, \"max_acc_iu_write\": %llu, "
                   "\"gc_pages\": %llu, \"acc_per_gc_page\": %.4f, \"flash_map_reads_per_gc_page\": %.5f, \"fold_pages\": %llu, \"acc_per_fold_page\": %.4f, "
                   "\"evict_folds\": %llu, \"evict_folds_per_host_write\": %.6f, \"acc_per_evict_fold\": %.2f, \"max_acc_evict_fold\": %llu, \"leaftl_mispredict_reads\": %llu, "
                   "\"acc_total_per_host_write\": %.4f},\n",
                   (unsigned long long)st.host_pages, R(st.dacc[1], st.host_pages), R(st.dfr[1], st.host_pages),
                   (unsigned long long)st.d_wp_ops, (unsigned long long)st.d_wp_max, (unsigned long long)st.d_wiu_ops, (unsigned long long)st.d_wiu_pages, (unsigned long long)st.d_wiu_max,
                   (unsigned long long)gcp, R(st.dacc[2], gcp), R(st.dfr[2], gcp), (unsigned long long)fdp, R(st.dacc[3], fdp),
                   (unsigned long long)st.d_evict, R(st.d_evict, st.host_pages), R(st.d_evict_acc, st.d_evict), (unsigned long long)st.d_evict_max, (unsigned long long)st.d_lf_mispredict,
                   R(st.dacc[1] + st.dacc[2] + st.dacc[3], st.host_pages));
        }
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
    void read_lookup(u64) {}
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
    void read_lookup(u64) {}
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
    u64 NIUw, Lw;              // workload footprint (first foot_frac of the IUs); = NIU, L by default
    Runner(S& sim) : s(sim), c(sim.c), L(sim.L), NIU(sim.NIU), U(sim.U) {
        NIUw = NIU; Lw = L;
        if (c.foot_frac < 1.0) { NIUw = std::max<u64>(1, (u64)(NIU * c.foot_frac)); Lw = NIUw * U; }
    }
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
            cdf.resize(NIUw);
            double acc = 0;
            for (u64 r = 0; r < NIUw; ++r) { acc += 1.0 / std::pow((double)(r + 1), c.zipf); cdf[r] = acc; }
            for (auto& x : cdf) x /= acc;
            rank2iu.resize(NIUw);
            for (u64 i = 0; i < NIUw; ++i) rank2iu[i] = (u32)i;
            if (c.zipf_shuffle) std::shuffle(rank2iu.begin(), rank2iu.end(), s.rng);
        }
    }
    u64 sample_iu() {
        if (c.zipf <= 0) return s.rng() % NIUw;
        double u = std::uniform_real_distribution<double>(0.0, 1.0)(s.rng);
        u64 r = std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin();
        if (r >= NIUw) r = NIUw - 1;
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
            emit_host(iu * U + start, off - start);
        }
        wb_pages -= __builtin_popcount(bm);
    }
    FILE* tracef = nullptr; u64 trace_pages = 0, trace_clipped = 0;
    void emit_host(u64 lpn, u64 n) {
        if (tracef) { for (u64 i = 0; i < n; ++i) fprintf(tracef, "0 1 %llu 4096\n", (unsigned long long)(lpn + i)); trace_pages += n; }
        s.host_write(lpn, n);
    }
    void wb_write(u64 lpn, u64 n) {
        s.st.host_submitted += n;
        if (!c.wbuf_pages) { emit_host(lpn, n); return; }
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
            if (__builtin_popcount(nb) == U) { wb_lru.erase(it->second.first); wb.erase(it); emit_host(iu * U, U); wb_pages -= U; }
            cur += w;
            while (wb_pages > c.wbuf_pages && !wb_lru.empty()) {           // evict LRU IU as fragments
                u64 v = wb_lru.back(); wb_lru.pop_back(); u32 bm = wb[v].second; wb.erase(v); wb_flush_iu(v, bm);
            }
        }
    }
    void read_page(u64 l) {   // dram_stats: buffered pages are served from the write buffer
        auto it = wb.find(l / U);
        if (it != wb.end() && (it->second.second >> (l % U) & 1)) { s.st.d_rd_buf++; return; }
        s.read_lookup(l);
    }
    std::mt19937_64 rrng{0x5eed5eedULL};
    void read_probe() {       // synthetic mode: one read page per write request, drawn from the write distribution
        u64 iu;
        if (c.zipf <= 0) iu = rrng() % NIUw;
        else { double u = std::uniform_real_distribution<double>(0.0, 1.0)(rrng); u64 r = std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin(); if (r >= NIUw) r = NIUw - 1; iu = rank2iu[r]; }
        read_page(std::min<u64>(iu * U + rrng() % U, L - 1));
    }
    void wb_drain() { while (!wb_lru.empty()) { u64 v = wb_lru.back(); wb_lru.pop_back(); u32 bm = wb[v].second; wb.erase(v); wb_flush_iu(v, bm); } }
    // append streams (WAL-like): app_streams circular regions of app_region/app_streams bytes each, written
    // round-robin in 2^app_k-page requests, every request issued app_rep times before its stream advances
    u64 app_done = 0, app_rr = 0; std::vector<u64> app_pos;
    void app_step(u64& done) {
        u64 n = 1ull << c.app_k;
        u64 ns = (u64)std::max(1, c.app_streams);
        u64 per = std::max<u64>(n, (c.app_region / 4096 / ns) / n * n);
        u64 base = c.app_off >= 0 ? (u64)(c.app_off * L) : Lw;
        base -= base % U;
        if (base + per * ns > L) DIE("append region [%llu, +%llu) exceeds the device", (unsigned long long)base, (unsigned long long)(per * ns));
        if (app_pos.empty()) app_pos.assign(ns, 0);
        u64 k = app_rr++ % ns;
        u64 lpn = base + k * per + app_pos[k];
        for (int r = 0; r < std::max(1, c.app_rep); ++r) { wb_write(lpn, n); done += n; app_done += n; }
        app_pos[k] += n; if (app_pos[k] + n > per) app_pos[k] = 0;
    }
    void run_workload(u64 pages) {
        u64 done = 0;
        while (done < pages) {
            if (c.app_frac > 0 && (double)app_done < c.app_frac * (double)(done + 1)) { app_step(done); continue; }
            u64 iu = sample_iu();
            int k;
            if (size_cdf.empty()) k = c.min_k + (int)(s.rng() % (u64)(c.max_k - c.min_k + 1));
            else { double u = std::uniform_real_distribution<double>(0.0, 1.0)(s.rng); k = c.min_k + (int)(std::lower_bound(size_cdf.begin(), size_cdf.end(), u) - size_cdf.begin()); if (k > c.max_k) k = c.max_k; }
            u64 n = 1ull << k;
            u64 lpn = iu * U + (s.rng() % U);
            if (c.seq_pattern) { lpn = seq_pos; seq_pos += n; if (seq_pos >= L) seq_pos = 0; }
            if (lpn + n > Lw) lpn = Lw - n;
            wb_write(lpn, n);
            done += n;
            if (c.dram_stats && s.measuring) read_probe();
        }
    }
    void seq_fill() {
        const u64 chunk = 64;  // 256K sequential writes
        u64 Lf = L;
        if (c.fill_frac < 1.0) { Lf = (u64)(L * c.fill_frac); Lf -= Lf % U; }
        for (u64 p = 0; p < Lf; p += chunk) wb_write(p, std::min(chunk, Lf - p));
    }

    // Real-trace replay: writes only, LBAs folded into the device by modulo, fed through the
    // same write buffer. Pass 1 counts write pages so the warmup/measure split is exact.
    void run_trace() {
        FILE* f = fopen(c.trace_file.c_str(), "r");
        if (!f) DIE("cannot open trace %s", c.trace_file.c_str());
        char line[256]; u64 total_pages = 0;
        while (fgets(line, sizeof line, f)) {
            char op; long long off, len;
            if (sscanf(line, "%*[^,],%c,%lld,%lld", &op, &off, &len) != 3 || op != 'W') continue;
            u64 p0 = (u64)off / 4096, p1 = ((u64)off + (u64)len + 4095) / 4096;
            total_pages += p1 - p0;
        }
        u64 warm = c.trace_warmup_pages >= 0 ? (u64)c.trace_warmup_pages : (u64)(total_pages * c.trace_warmup);
        fprintf(stderr, "[trace] %s: %llu write pages (%.1f GiB, %.2fx device), warmup %.1f%%\n",
                c.trace_file.c_str(), (unsigned long long)total_pages, total_pages / 262144.0,
                (double)total_pages / L, 100.0 * (double)warm / (double)std::max<u64>(1, total_pages));
        rewind(f);
        // Dense remap at 1 MiB granularity: regions get dense slots in first-touch order, offsets
        // inside a region are preserved (so request alignment survives). Sparse multi-TB address
        // spaces thus compact into the device instead of being folded destructively by modulo.
        const u64 RPG = (1u << 20) / 4096;                  // pages per region
        std::unordered_map<u64, u64> rmap_dense; u64 next_region = 0;
        u64 done = 0; bool measuring_started = false;
        while (fgets(line, sizeof line, f)) {
            char op; long long off, len;
            if (sscanf(line, "%*[^,],%c,%lld,%lld", &op, &off, &len) != 3) continue;
            if (op == 'R' && c.dram_stats && measuring_started) {    // read through the mapping (counting only)
                u64 q0 = (u64)off / 4096, q1 = ((u64)off + (u64)len + 4095) / 4096;
                for (u64 q = q0; q < q1; ++q) {
                    u64 l = q;
                    if (c.trace_remap) { auto it = rmap_dense.find(q / RPG); if (it == rmap_dense.end()) { s.st.d_rd_unmapped++; continue; } l = it->second * RPG + q % RPG; }
                    if (l >= L) continue;
                    read_page(l);
                }
                continue;
            }
            if (op != 'W') continue;
            u64 p0 = (u64)off / 4096, p1 = ((u64)off + (u64)len + 4095) / 4096;
            if (c.trace_stop_pages >= 0 && done >= (u64)c.trace_stop_pages) break;
            if (!c.trace_remap) {                                   // original addresses
                if (p1 > L && c.trace_clip) {                       // device of L - L%U pages: drop the tail pages beyond it,
                    if (p0 < L) wb_write(p0, L - p0);               // but keep counting them so the warm-up / stop markers align
                    trace_clipped += p1 - std::max(p0, (u64)L); done += p1 - p0; continue;
                }
                if (p1 > L) DIE("trace write [%llu, %llu) beyond the device (%llu pages)", (unsigned long long)p0, (unsigned long long)p1, (unsigned long long)L);
                wb_write(p0, p1 - p0); done += p1 - p0;
            } else
            for (u64 cur = p0; cur < p1; ) {                       // per-source-region segments
                u64 r = cur / RPG, seg_end = std::min(p1, (r + 1) * RPG);
                auto it = rmap_dense.find(r);
                if (it == rmap_dense.end()) it = rmap_dense.emplace(r, next_region++).first;
                u64 lpn = it->second * RPG + (cur - r * RPG), n = seg_end - cur;
                if (lpn + n > L) DIE("trace dense space exceeds device (%llu regions); raise capacity",
                                     (unsigned long long)next_region);
                wb_write(lpn, n);
                done += n; cur = seg_end;
            }
            if (!measuring_started && done >= warm) {
                s.verify();
                fprintf(stderr, "[trace] warmup done (%llu pages), reset stats\n", (unsigned long long)done);
                s.st.reset(U); s.measuring = true; measuring_started = true;
                if (!c.out_ts.empty() && !s.ts) { s.ts_interval = c.ts_interval ? c.ts_interval : std::max<u64>(1, L / 200); s.ts_next = s.ts_interval; s.ts = fopen(c.out_ts.c_str(), "w"); if (s.ts) s.ts_header(); }
            }
        }
        fclose(f);
        if (c.trace_remap)
            fprintf(stderr, "[trace] dense space: %llu MiB regions (%.1f GiB), device utilization %.1f%%\n",
                    (unsigned long long)next_region, next_region / 1024.0, 100.0 * next_region * RPG / L);
        else fprintf(stderr, "[trace] identity addresses: %llu write pages replayed (%llu clipped beyond L)\n", (unsigned long long)done, (unsigned long long)trace_clipped);
        wb_drain();
        if (s.ts) { s.ts_sample(); fclose(s.ts); s.ts = nullptr; }
        s.verify();
        s.report();
    }

    void run() {
        if (!c.trace_file.empty()) {
            if (!c.out_trace.empty()) { tracef = fopen(c.out_trace.c_str(), "w"); if (!tracef) DIE("cannot open out_trace"); }
            if (c.seq_fill) { fprintf(stderr, "[phase] seq fill\n"); seq_fill(); }
            run_trace();
            if (tracef) fclose(tracef);
            return;
        }
        build_workload();
        if (!c.out_trace.empty()) { tracef = fopen(c.out_trace.c_str(), "w"); if (!tracef) DIE("cannot open out_trace"); }
        if (c.seq_fill) { fprintf(stderr, "[phase] seq fill\n"); seq_fill(); }
        if (c.age_x > 0) { fprintf(stderr, "[phase] aging %.2fx\n", c.age_x); run_workload((u64)(L * c.age_x)); }
        s.verify();
        wb_drain();
        if (tracef) fprintf(stderr, "[trace] pages before measure: %llu\n", (unsigned long long)trace_pages);
        fprintf(stderr, "[phase] reset stats, measure %.2fx\n", c.measure_x);
        s.st.reset(U);
        s.measuring = true;
        s.ts_interval = c.ts_interval ? c.ts_interval : std::max<u64>(1, L / 200);
        s.ts_next = s.ts_interval;
        if (!c.out_ts.empty()) { s.ts = fopen(c.out_ts.c_str(), "w"); if (s.ts) s.ts_header(); }
        run_workload((u64)(L * c.measure_x));
        wb_drain();
        if (s.ts) { s.ts_sample(); fclose(s.ts); }
        if (tracef) { fprintf(stderr, "[trace] total pages: %llu\n", (unsigned long long)trace_pages); fclose(tracef); }
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
        else if (k == "out_trace") c.out_trace = v;
        else if (k == "trace") c.trace_file = v;
        else if (k == "trace_warmup") c.trace_warmup = atof(v.c_str());
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
        else if (k == "fine_full") c.fine_full = atoi(v.c_str()) != 0;
        else if (k == "gc_select") c.gc_select = v;
        else if (k == "coarse_class") c.coarse_class = atoi(v.c_str());
        else if (k == "cgc_vict") c.cgc_vict = v;
        else if (k == "zipf_shuffle") c.zipf_shuffle = atoi(v.c_str()) != 0;
        else if (k == "last_seq_frac") c.last_seq_frac = atof(v.c_str());
        else if (k == "last_seq_thresh") c.last_seq_thresh = strtoull(v.c_str(), nullptr, 10);
        else if (k == "last_hot_frac") c.last_hot_frac = atof(v.c_str());
        else if (k == "last_fixed_caps") c.last_fixed_caps = atoi(v.c_str()) != 0;
        else if (k == "cache_bytes") c.cache_bytes = parse_bytes(v);
        else if (k == "dftl_tpbuf") c.dftl_tpbuf = atoi(v.c_str()) != 0;
        else if (k == "adapt") c.adapt = atoi(v.c_str());
        else if (k == "fine_index") c.fine_index = v;
        else if (k == "map_budget") c.map_budget = atof(v.c_str());
        else if (k == "rec_block") c.rec_block = atoi(v.c_str());
        else if (k == "uni_onefront") c.uni_onefront = atoi(v.c_str());
        else if (k == "leaftl_gamma") c.leaftl_gamma = atof(v.c_str());
        else if (k == "leaftl_batch") c.leaftl_batch = atoi(v.c_str());
        else if (k == "leaftl_gcsort") c.leaftl_gcsort = atoi(v.c_str()) != 0;
        else if (k == "bitmap_cache_bytes") c.bitmap_cache_bytes = parse_bytes(v);
        else if (k == "mu_parts") c.mu_parts = atoi(v.c_str());
        else if (k == "mu_max_extent") c.mu_max_extent = atoi(v.c_str());
        else if (k == "mu_leaf_fanout") c.mu_leaf_fanout = atoi(v.c_str());
        else if (k == "janus_u") c.janus_u = atof(v.c_str());
        else if (k == "janus_cap") c.janus_cap = atoi(v.c_str());
        else if (k == "last_k") c.last_k = strtoull(v.c_str(), nullptr, 10);
        else if (k == "fill_frac") c.fill_frac = atof(v.c_str());
        else if (k == "foot_frac") c.foot_frac = atof(v.c_str());
        else if (k == "app_frac") c.app_frac = atof(v.c_str());
        else if (k == "app_region") c.app_region = parse_bytes(v);
        else if (k == "app_off") c.app_off = atof(v.c_str());
        else if (k == "app_k") c.app_k = atoi(v.c_str());
        else if (k == "app_rep") c.app_rep = atoi(v.c_str());
        else if (k == "app_streams") c.app_streams = atoi(v.c_str());
        else if (k == "fine_elastic") c.fine_elastic = atoi(v.c_str());
        else if (k == "elastic_keep") c.elastic_keep = atof(v.c_str());
        else if (k == "trace_remap") c.trace_remap = atoi(v.c_str());
        else if (k == "trace_warmup_pages") c.trace_warmup_pages = atoll(v.c_str());
        else if (k == "trace_stop_pages") c.trace_stop_pages = atoll(v.c_str());
        else if (k == "trace_clip") c.trace_clip = atoi(v.c_str());
        else if (k == "dram_stats") c.dram_stats = atoi(v.c_str());
        else if (k == "evict_pol") c.evict_pol = atoi(v.c_str());
        else if (k == "gc_fine_bias") c.gc_fine_bias = atof(v.c_str());
        else { fprintf(stderr, "unknown key %s\n", k.c_str()); return 1; }
    }
    if (c.mode == "cftl") c.mode = "dftl";  // alias
    if (c.mode == "sepbit" && c.T == 2) c.T = 8;  // GC writes fan out to classes 2-5: reserve must cover 6 open segments
    if (c.U > 32) { fprintf(stderr, "U > 32 unsupported (bitmap)\n"); return 1; }
    if (c.P == 0) { u64 pages = c.capacity / 4096; u64 p = pages * 3 / 1024; p -= p % (64 * c.U); if (p < (u64)(64 * c.U)) p = 64 * c.U; c.P = (int)p; }
    if (c.mode == "fast" || c.mode == "last" || c.mode == "janus") { HybridSim s(c); Runner<HybridSim> r(s); r.run(); }
    else if (c.mode == "mu") { MuSim s(c); Runner<MuSim> r(s); r.run(); }
    else { Sim s(c); Runner<Sim> r(s); r.run(); }
    return 0;
}
