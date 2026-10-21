#!/usr/bin/env python3
# gen.py GROUP TAG "extra params" [workload ...]  -> job lines "results/coarse_gc/GROUP/WL TAG args..." on stdout
# args = workload args (workloads.txt) + BASE (base_params.txt) + extra (later keys override earlier ones)
import sys
d='results/coarse_gc/'
W=dict(l.split(' ',1) for l in open(d+'workloads.txt').read().splitlines())
base=open(d+'base_params.txt').read().strip()
g,tag,extra=sys.argv[1:4]
wls=sys.argv[4:] or list(W)
for w in wls:
    print(f"{d}{g}/{w} {tag} {W[w]} {base} {extra}".rstrip())
