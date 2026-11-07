#!/usr/bin/env python3
# gen.py TAG "extra params" [workload ...] -> job lines "results/frag_bitmap/WL TAG args..." on stdout
# args = workload args (results/coarse_gc/workloads.txt) + results/coarse_gc/base_params.txt (V3 + spill=2 dram_stats=1,
# map_budget 0.125) + "spill_rp=1 spill_rp_clean=1" (= current DUET BASE, results/spill_rp rpc1) + extra (later keys override)
import sys
d = 'results/coarse_gc/'
W = dict(l.split(' ', 1) for l in open(d + 'workloads.txt').read().splitlines())
base = open(d + 'base_params.txt').read().strip() + ' spill_rp=1 spill_rp_clean=1'
tag, extra = sys.argv[1:3]
WL = ['spfresh', 'ycsba_d_t64', 'ycsbb_d_t64', 'ycsbfstd_d_t64', 'tpcc_d_t64', 'ali3', 'f73', 'f37']
for w in sys.argv[3:] or WL:
    print(f"results/frag_bitmap/{w} {tag} {W[w]} {base} {extra}".rstrip())
