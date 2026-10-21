#!/bin/bash
# Regression of the partial-fill fix on the full results/op7b matrix: V3 18.75% (tools/run_op7.sh P12) + $FIX,
# same workload arguments as op7b (64 GiB synthetic zipf with seq fill + 8x aging; real traces with seq fill + dense
# remap). Output results/op7_fix/<workload>/<tag>.json, compared with results/op7b/<workload>/V3_18.json.
#   FIX="fine_elastic=1" TAG=V3F tools/run_op7_fix.sh [workload ...]
cd "$(dirname "$0")/.."
S=${SIM:-./ftlsim/ftlsim}
FIX=${FIX:-fine_elastic=1}; TAG=${TAG:-V3F}
CPUS=${CPUS:-0-92:2}
V3="mode=proposed U=16 op=0.07 fine_extra=0 fine_quota=0.0429 fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2 adapt=2 uni_onefront=1 fine_index=cuckoo2 rec_block=1 map_budget=0.1875"
declare -A W=(
  [z0]="zipf=0" [z0.8]="zipf=0.8" [z0.99]="zipf=0.99" [z1.2]="zipf=1.2"
  [ali1]="trace=/home/sejun000/traces_dm/ali1.csv capacity=276605960192"
  [ali2]="trace=/home/sejun000/traces_dm/ali2.csv capacity=368702390272"
  [ali3]="trace=/home/sejun000/traces_dm/ali3.csv capacity=2587949531136"
  [ssd]="trace=/home/sejun000/traces_dm/ssd.csv capacity=423542915072"
  [kv_w1]="trace=/mnt/nvme_mounted/kvtrace/w1.packed.csv capacity=67001909248"
  [spfresh]="trace=/mnt/nvme_mounted/spfresh_trace/spf.packed.csv capacity=42970644480" )
for w in ${@:-z0 z0.8 z0.99 z1.2 ali1 ali2 ali3 ssd kv_w1 spfresh}; do
  mkdir -p results/op7_fix/$w
  taskset -c $CPUS $S ${W[$w]} $V3 map_budget=0.1875 $FIX tag=$TAG > results/op7_fix/$w/$TAG.json 2> results/op7_fix/$w/$TAG.log &
done
wait
for w in z0 z0.8 z0.99 z1.2 ali1 ali2 ali3 ssd kv_w1 spfresh; do
  python3 - "$w" "$TAG" <<'PY'
import json, sys
w, tag = sys.argv[1:]
def waf(p):
    try: return json.load(open(p))['WAF']
    except Exception: return float('nan')
a, b = waf(f'results/op7b/{w}/V3_18.json'), waf(f'results/op7_fix/{w}/{tag}.json')
print(f'{w:8s} V3_18={a:8.4f} {tag}={b:8.4f} ({(b / a - 1) * 100:+.2f}%)')
PY
done
