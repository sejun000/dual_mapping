#!/bin/bash
# OP 7% (fine pool carved from it) with fixed mapping-DRAM budgets as a fraction of the page map (L x 4 B):
#   P12 = 0.1875, P10 = 0.10, both the V3 proposed config (64K IU, static cuckoo2 table with fold on eviction, adapt=2);
#   DFTL / LeaFTL / mu-FTL get the same byte budgets as their cache. Baselines: page4k, SepBIT, IU+RMW, FAST.
#   tools/run_op7.sh <outdir> <workload-args...>   (workload-args: "trace=<csv> capacity=<bytes>" or "zipf=<z>")
#   FASTCAP=<bytes> FASTP=<pages>: FAST gets the auto block size of the real capacity and a capacity rounded up to
#   whole blocks (FAST truncates L to whole blocks; +0.1..0.4%).
cd "$(dirname "$0")/.."
OUT=$1; shift; mkdir -p "$OUT"
S=./ftlsim/ftlsim
L=$(python3 - "$@" <<'PY'
import sys
cap=64<<30
for a in sys.argv[1:]:
    if a.startswith('capacity='): cap=int(a.split('=')[1])
print(cap//4096)
PY
)
B12=$((L*4*1875/10000)); B10=$((L*4/10))
# V3: 64K IU, 8 B cuckoo entries (block-metadata recency), adaptive uniform mode (random-cache rule) with one fine frontier
V3="mode=proposed U=16 op=0.07 fine_extra=0 fine_quota=0.0429 fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2 adapt=2 uni_onefront=1 fine_index=cuckoo2 rec_block=1"
P12="$V3 map_budget=0.1875"
P10="$V3 map_budget=0.10"
run() { local tag=$1; shift; $S "$@" tag=$tag > $OUT/$tag.json 2> $OUT/$tag.log; }
WL=("$@")
FWL=("$@"); [ -n "$FASTCAP" ] && FWL=("${@/capacity=*/capacity=$FASTCAP}" P=$FASTP)
run P12 ${WL[@]} $P12 &
run P10 ${WL[@]} $P10 &
run page4k ${WL[@]} mode=page4k op=0.07 &
run sepbit ${WL[@]} mode=sepbit op=0.07 &
run iu_rmw ${WL[@]} mode=iu_rmw op=0.07 &
run fast ${FWL[@]} mode=fast op=0.07 &
run mu12 ${WL[@]} mode=mu op=0.07 cache_bytes=$B12 &
run mu10 ${WL[@]} mode=mu op=0.07 cache_bytes=$B10 &
run dftl12 ${WL[@]} mode=dftl op=0.07 cache_bytes=$B12 dftl_tpbuf=1 &
run dftl10 ${WL[@]} mode=dftl op=0.07 cache_bytes=$B10 dftl_tpbuf=1 &
run leaftl12 ${WL[@]} mode=leaftl op=0.07 cache_bytes=$B12 &
run leaftl10 ${WL[@]} mode=leaftl op=0.07 cache_bytes=$B10 &
wait
