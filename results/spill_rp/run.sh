#!/bin/bash
# spill read-promotion (ftlsim spill_rp=K [spill_rp_rate=R]) vs BASE (spill=2 dram_stats=1, results/coarse_gc params).
# Job lines "OUTDIR TAG args..." (file arg or stdin) run with results/spill_rp/ftlsim.bin (= ftlsim/ftlsim.cpp with spill_rp)
# on NUMA-node-0 CPUs 0-40 (even), JOBS parallel (default 12). Skips jobs whose json exists.
# e.g. python3 results/spill_rp/gen.py rp1 "spill_rp=1" | results/spill_rp/run.sh
cd /home/sejun000/dual_mapping
S=${SIM:-./results/spill_rp/ftlsim.bin}
cat ${1:-/dev/stdin} | xargs -P ${JOBS:-12} -L 1 bash -c 'o=$0; tag=$1; shift; mkdir -p $o; [ -s $o/$tag.json ] && exit 0; taskset -c 0-40:2 '"$S"' "$@" tag=$tag > $o/$tag.json 2> $o/$tag.log || mv $o/$tag.json $o/$tag.failed'
