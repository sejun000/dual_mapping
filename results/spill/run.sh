#!/bin/bash
# cuckoo2 "spill fine maps to NAND, fold lazily" evaluation (ftlsim spill=1/2, fold_on_read=1..4; all default off).
#   jobs.txt : canonical bit-identity checks (canon/), per workload s1/s2/s1r1/s1r3 at map_budget 0.125 / 0.1875
#              (aged app traces) or 0.125 (ali3 / spfresh at the NVMeVirt geometry), prop_d (default proposed with
#              dram_stats=1: must equal results/sim_budget prop_0.125) and dftl_d (DFTL map reads per host read; its WAF
#              changes with the read replay), s1nod (spill without dram_stats: WAF must equal s1).
#   jobs2.txt: fold_on_read guards s1r2 / s1r4.
# Arguments are those of results/sim_budget/jobs.txt (tools/sim_budget.sh). Baselines (prop, page4k, dftl, leaftl,
# prop_1.0) are read from results/sim_budget. Summary: python3 results/spill/summarize.py -> SUMMARY.txt
# Run: [JOBSF=results/spill/jobs2.txt] [JOBS=20] results/spill/run.sh   (NUMA node 0 CPUs, never 94/95)
cd /home/sejun000/dual_mapping
S=${SIM:-./ftlsim/ftlsim}
cat ${JOBSF:-results/spill/jobs.txt} | xargs -P ${JOBS:-20} -L 1 bash -c 'o=$0; tag=$1; shift; mkdir -p $o; [ -s $o/$tag.json ] && exit 0; taskset -c 0-92:2 '"$S"' "$@" tag=$tag > $o/$tag.json 2> $o/$tag.log'
