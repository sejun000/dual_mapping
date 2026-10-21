#!/bin/bash
# Run job lines "OUTDIR TAG args..." (JOBSF, default stdin) with ftlsim_cgc on NUMA-node-0 CPUs 0-40 (even), JOBS parallel.
# Skips jobs whose json exists. Usage: results/coarse_gc/run.sh jobs.txt [JOBS=12]
cd /home/sejun000/dual_mapping
S=${SIM:-./ftlsim/ftlsim_cgc}
cat ${1:-/dev/stdin} | xargs -P ${JOBS:-12} -L 1 bash -c 'o=$0; tag=$1; shift; mkdir -p $o; [ -s $o/$tag.json ] && exit 0; taskset -c 0-40:2 '"$S"' "$@" tag=$tag > $o/$tag.json 2> $o/$tag.log || mv $o/$tag.json $o/$tag.failed'
