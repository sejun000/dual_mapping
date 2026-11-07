#!/bin/bash
# fragment-location hints (ftlsim pbm=1 / ssum=1) vs BASE (spill=2 spill_rp=1 spill_rp_clean=1 dram_stats=1, map_budget 0.125).
# Job lines "OUTDIR TAG args..." (file arg or stdin) run with results/frag_bitmap/ftlsim.bin (= ftlsim/ftlsim.cpp with pbm/ssum)
# on NUMA-node-0 CPUs 0-40 (even), JOBS parallel (default 12). Skips jobs whose json exists.
# e.g. python3 results/frag_bitmap/gen.py pbm "pbm=1" | results/frag_bitmap/run.sh
cd /home/sejun000/dual_mapping
S=${SIM:-./results/frag_bitmap/ftlsim.bin}
cat ${1:-/dev/stdin} | xargs -P ${JOBS:-12} -L 1 bash -c 'o=$0; tag=$1; shift; mkdir -p $o; [ -s $o/$tag.json ] && exit 0; taskset -c 0-40:2 '"$S"' "$@" tag=$tag > $o/$tag.json 2> $o/$tag.log || mv $o/$tag.json $o/$tag.failed'
