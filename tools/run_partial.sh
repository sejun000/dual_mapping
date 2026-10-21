#!/bin/bash
# Partially filled device (file-system-like) synthetic runs at the NVMeVirt 44G geometry
# (L = 10,798,160 pages = 44,229,263,360 B, P = 30976 pages/line, NB = 373 lines, OP 7%, U = 16).
#   tools/run_partial.sh <outdir> <workload key=value ...>
#   e.g. tools/run_partial.sh results/partial/f76_u fill_frac=0.76 foot_frac=0.73 min_k=0 max_k=2
# POLS (default "V3 V3F page4k dftl leaftl"): V3 = canonical V3 18.75% (as tools/run_op7.sh P12);
#   V3F = V3 + the partial-fill fix ($FIX, default below); DFTL/LeaFTL cache = 18.75% of L x 4 B (B18 = 8,098,620).
# CPUS: taskset list (default NUMA node 0 even CPUs 0-92; 94/95 belong to NVMeVirt).
cd "$(dirname "$0")/.."
OUT=$1; shift; mkdir -p "$OUT"
S=${SIM:-./ftlsim/ftlsim}
GEO="capacity=44229263360 P=30976 op=0.070001602125"
B18=8098620
V3="mode=proposed U=16 fine_extra=0 fine_quota=0.0429 fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2 adapt=2 uni_onefront=1 fine_index=cuckoo2 rec_block=1 map_budget=0.1875"
FIX=${FIX:-fine_elastic=1}
CPUS=${CPUS:-0-92:2}
run() { local tag=$1; shift; taskset -c $CPUS $S $GEO "$@" tag=$tag > $OUT/$tag.json 2> $OUT/$tag.log; }
for p in ${POLS:-V3 V3F page4k dftl leaftl}; do
  case $p in
    V3)     run V3 "$@" $V3 & ;;
    V3F)    run V3F "$@" $V3 $FIX & ;;
    V3F*)   eval "F=\$FIX_${p#V3F}"; run $p "$@" $V3 $F & ;;
    page4k) run page4k "$@" mode=page4k & ;;
    dftl)   run dftl "$@" mode=dftl cache_bytes=$B18 dftl_tpbuf=1 & ;;
    leaftl) run leaftl "$@" mode=leaftl cache_bytes=$B18 & ;;
    sepbit) run sepbit "$@" mode=sepbit & ;;
  esac
done
wait
