#!/bin/bash
# Simulator check of the mapping-DRAM budget question before re-running NVMeVirt:
# proposed (V3 + onefront_fix=1 pool_cap=4, the NVMeVirt config) at 12.5 / 18.75 / 25 / 100% (100% ~ upper bound of
# spilling fine maps to NAND instead of folding), page4k, DFTL and LeaFTL at 12.5 and 18.75%.
# Workloads: sampled real traces at the NVMeVirt device geometry (as results/op7s/run_devgeom.sh) and the captured
# application traces in the aged condition (as tools/apps_trace_sim.sh). Output results/sim_budget/<wl>/<tag>.json.
cd /home/sejun000/dual_mapping
S=./ftlsim/ftlsim; OUT=results/sim_budget
V="mode=proposed U=16 fine_extra=0 fine_quota=0.0429 fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2 adapt=2 uni_onefront=1 fine_index=cuckoo2 rec_block=1 onefront_fix=1 pool_cap=4"
jobs() {
  local wl=$1 W=$2 L=$3
  local B12=$((L * 4 * 1250 / 10000)) B18=$((L * 4 * 1875 / 10000))
  for b in 0.125 0.1875 0.25 1.0; do echo "$OUT/$wl prop_$b $W $V map_budget=$b"; done
  echo "$OUT/$wl page4k $W mode=page4k"
  echo "$OUT/$wl dftl_0.125 $W mode=dftl cache_bytes=$B12 dftl_tpbuf=1"
  echo "$OUT/$wl dftl_0.1875 $W mode=dftl cache_bytes=$B18 dftl_tpbuf=1"
  echo "$OUT/$wl leaftl_0.125 $W mode=leaftl cache_bytes=$B12"
  echo "$OUT/$wl leaftl_0.1875 $W mode=leaftl cache_bytes=$B18"
}
{
D=/mnt/nvme_mounted/dm_trace_sampled
for t in ali3 spfresh ali1 ssd; do
  CAP=$(python3 -c "import json;print(json.load(open('$D/$t.json'))['capacity_bytes'])")
  M=$(python3 tools/nvmev_geom.py --fit $CAP)
  read L P OP < <(python3 -c "
import sys;sys.path.insert(0,'tools');from nvmev_geom import geom,parse;g=geom(parse('$M'))
print(g['L'],g['P'],'%.12f'%((373*g['P']+0.5)/g['L']-1))")
  jobs $t "trace=$D/$t.csv capacity=$((L * 4096)) P=$P op=$OP" $L
done
A=/mnt/nvme_mounted/dm_apptrace
for d in $A/*/; do
  wl=$(basename $d); [ -s $d/trace.json ] || continue
  read CAP WARM STOP < <(python3 - $d/trace.json <<'PY'
import json, sys
j = json.load(open(sys.argv[1])); m = {x['name']: x for x in j.get('markers', [])}
w = m.get('dm_measure_prefill_done', m.get('dm_measure_start')); s = m.get('dm_measure_end')
print(j['capacity_bytes'], w['write_pages_before'], s['write_pages_before'])
PY
)
  jobs $wl "trace=$d/trace.csv trace_remap=0 seq_fill=1 capacity=$CAP P=30976 op=0.070001602125 trace_warmup_pages=$WARM trace_stop_pages=$STOP" $((CAP / 4096))
done
} > $OUT/jobs.txt
wc -l < $OUT/jobs.txt
cat $OUT/jobs.txt | xargs -P ${JOBS:-36} -L 1 bash -c 'o=$0; tag=$1; shift; mkdir -p $o; taskset -c 0-92 '"$S"' "$@" tag=$tag > $o/$tag.json 2> $o/$tag.log'
python3 - <<'PY'
import json, glob, os
tags = ['prop_0.125', 'prop_0.1875', 'prop_0.25', 'prop_1.0', 'page4k', 'dftl_0.125', 'dftl_0.1875', 'leaftl_0.125', 'leaftl_0.1875']
out = ['WAF (ftlsim). prop_1.0 ~ upper bound of spilling fine maps to NAND instead of folding.',
       f"{'workload':16s} " + ' '.join(f'{t:>13s}' for t in tags)]
for d in sorted(glob.glob('results/sim_budget/*/')):
    row = []
    for t in tags:
        try: row.append(f"{json.load(open(d + t + '.json'))['WAF']:13.3f}")
        except Exception: row.append(f"{'-':>13s}")
    out.append(f'{os.path.basename(d[:-1]):16s} ' + ' '.join(row))
open('results/sim_budget/SUMMARY.txt', 'w').write('\n'.join(out) + '\n'); print('\n'.join(out))
PY
