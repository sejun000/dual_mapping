#!/usr/bin/env python3
# gen.py TAG "extra params" [workload ...] -> job lines "results/spill_rp/WL TAG args..." on stdout
# args = workload args (results/coarse_gc/workloads.txt) + BASE (results/coarse_gc/base_params.txt: V3 + spill=2
# dram_stats=1, map_budget 0.125) + extra (later keys override earlier ones)
import sys
d = 'results/coarse_gc/'
W = dict(l.split(' ', 1) for l in open(d + 'workloads.txt').read().splitlines())
base = open(d + 'base_params.txt').read().strip()
tag, extra = sys.argv[1:3]
WL = ['spfresh', 'ycsba_d_t64', 'ycsbb_d_t64', 'ycsbfstd_d_t64', 'tpcc_d_t64', 'ali3', 'f73', 'f37']
for w in sys.argv[3:] or WL:
    print(f"results/spill_rp/{w} {tag} {W[w]} {base} {extra}".rstrip())
