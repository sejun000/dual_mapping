#!/bin/bash
# CoMap (mode=comap) + DFTL + FAST (fast_cap=1) ftlsim baselines at 1/8 mapping DRAM on the 8 workloads of
# results/frag_bitmap/jobs1.txt (their BASE job lines with the policy options replaced; see SUMMARY.txt).
# Job lines "OUTDIR TAG args..." (file arg or stdin) run with results/comap_sim/ftlsim.bin (= ftlsim/ftlsim.cpp with mode=comap)
# on NUMA-node-0 CPUs 4-40 (even), JOBS parallel (default 8). Skips jobs whose json exists.
cd /home/sejun000/dual_mapping
S=${SIM:-./results/comap_sim/ftlsim.bin}
cat ${1:-/dev/stdin} | xargs -P ${JOBS:-8} -L 1 bash -c 'o=$0; tag=$1; shift; mkdir -p $o; [ -s $o/$tag.json ] && exit 0; taskset -c 4-40:2 '"$S"' "$@" tag=$tag > $o/$tag.json 2> $o/$tag.log || mv $o/$tag.json $o/$tag.failed'
